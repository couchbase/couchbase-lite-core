//
// SQLiteDataFile+Indexes.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "SQLiteDataFile.hh"
#include "SQLiteKeyStore.hh"
#include "SQLite_Internal.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "FleeceImpl.hh"
#include "Doc.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "sqlite3.h"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {


#pragma mark - INDEX-TABLE MANAGEMENT:


    bool SQLiteDataFile::indexTableExists() {
        string sql;
        return getSchema("indexes", "table", "indexes", sql);
    }


    void SQLiteDataFile::ensureIndexTableExists() {
        if (indexTableExists())
            return;

        if (!options().upgradeable && _schemaVersion < SchemaVersion::WithIndexTable)
            error::_throw(error::CantUpgradeDatabase,
                          "Accessing indexes requires upgrading the database schema");

        Assert(inTransaction());

        int userVersion = _sqlDb->execAndGet("PRAGMA user_version");
        if (!options().upgradeable && userVersion < 301)
            error::_throw(error::CantUpgradeDatabase, "Database needs upgrade of index metadata");

        LogTo(DBLog, "Upgrading database to use 'indexes' table...");
        _exec("CREATE TABLE indexes (name TEXT PRIMARY KEY, type INTEGER NOT NULL,"
                                  " keyStore TEXT NOT NULL, expression TEXT, indexTableName TEXT)");
        ensureSchemaVersionAtLeast(SchemaVersion::WithIndexTable); // Backward-incompatible with version 2.0/2.1

        for (auto &spec : getIndexesOldStyle())
            registerIndex(spec, spec.keyStoreName, spec.indexTableName);
    }


    void SQLiteDataFile::registerIndex(const litecore::IndexSpec &spec,
                                       const string &keyStoreName, const string &indexTableName)
    {
        SQLite::Statement stmt(*this, "INSERT INTO indexes (name, type, keyStore, expression, indexTableName) "
                                      "VALUES (?, ?, ?, ?, ?)");
        stmt.bindNoCopy(1, spec.name);
        stmt.bind(      2, spec.type);
        stmt.bindNoCopy(3, keyStoreName);
        stmt.bindNoCopy(4, (char*)spec.expression.buf, (int)spec.expression.size);
        if (spec.type != IndexSpec::kValue)
            stmt.bindNoCopy(5, indexTableName);
        LogStatement(stmt);
        stmt.exec();
    }



    void SQLiteDataFile::unregisterIndex(slice indexName) {
        SQLite::Statement stmt(*this, "DELETE FROM indexes WHERE name=?");
        stmt.bindNoCopy(1, (char*)indexName.buf, (int)indexName.size);
        LogStatement(stmt);
        stmt.exec();
    }


#pragma mark - CREATING INDEXES:


    bool SQLiteDataFile::createIndex(const litecore::IndexSpec &spec,
                                     SQLiteKeyStore *keyStore,
                                     const string &indexTableName,
                                     const string &indexSQL)
    {
        ensureIndexTableExists();
        if (auto existingSpec = getIndex(spec.name)) {
            if (existingSpec->type == spec.type && existingSpec->keyStoreName == keyStore->name()) {
                bool same;
                if (spec.type == IndexSpec::kFullText)
                    same = schemaExistsWithSQL(indexTableName, "table", indexTableName, indexSQL);
                else
                    same = schemaExistsWithSQL(spec.name, "index", indexTableName, indexSQL);
                if (same)
                    return false;       // This is a duplicate of an existing index; do nothing
            }
            // Existing index is different, so delete it first:
            deleteIndex(*existingSpec);
        }
        LogTo(QueryLog, "Creating %s index: %s", spec.typeName(), indexSQL.c_str());
        exec(indexSQL);
        registerIndex(spec, keyStore->name(), indexTableName);
        return true;
    }


#pragma mark - DELETING INDEXES:


    void SQLiteDataFile::deleteIndex(const SQLiteIndexSpec &spec) {
        ensureIndexTableExists();
        LogTo(QueryLog, "Deleting %s index '%s'",
              spec.typeName(), spec.name.c_str());
        unregisterIndex(spec.name);
        if (spec.type != IndexSpec::kFullText)
            exec(CONCAT("DROP INDEX IF EXISTS \"" << spec.name << "\""));
        if (!spec.indexTableName.empty())
            garbageCollectIndexTable(spec.indexTableName);
    }


    // Drops unnested-array tables that no longer have any indexes on them.
    void SQLiteDataFile::garbageCollectIndexTable(const string &tableName) {
        {
            SQLite::Statement stmt(*this, "SELECT name FROM indexes WHERE indexTableName=?");
            stmt.bind(1, tableName);
            if (stmt.executeStep())
                return;
        }

        LogTo(QueryLog, "Dropping unused index table '%s'", tableName.c_str());
        exec(CONCAT("DROP TABLE \"" << tableName << "\""));

        stringstream sql;
        static const char* kTriggerSuffixes[] = {"ins", "del", "upd", "preupdate", "postupdate",
                                                 nullptr};
        for (int i = 0; kTriggerSuffixes[i]; ++i) {
            sql << "DROP TRIGGER IF EXISTS \"" << tableName << "::" << kTriggerSuffixes[i] << "\";";
        }
        exec(sql.str());
    }


#pragma mark - GETTING INDEX INFO:


    vector<SQLiteIndexSpec> SQLiteDataFile::getIndexes(const KeyStore *store) {
        if (indexTableExists()) {
            vector<SQLiteIndexSpec> indexes;
            SQLite::Statement stmt(*this, "SELECT name, type, expression, keyStore, indexTableName "
                                          "FROM indexes ORDER BY name");
            while(stmt.executeStep()) {
                string keyStoreName = stmt.getColumn(3);
                if (!store || keyStoreName == store->name())
                    indexes.emplace_back(specFromStatement(stmt));
            }
            return indexes;
        } else {
            return getIndexesOldStyle(store);
        }
    }


    // Finds the indexes the old 2.0/2.1 way, without using the 'indexes' table.
    vector<SQLiteIndexSpec> SQLiteDataFile::getIndexesOldStyle(const KeyStore *store) {
        vector<SQLiteIndexSpec> indexes;
        // value indexes:
        SQLite::Statement getIndex(*this, "SELECT name, tbl_name FROM sqlite_master "
                                          "WHERE type = 'index' "
                                          "AND tbl_name LIKE 'kv_%' "
                                          "AND name NOT LIKE 'kv_%_blobs' "
                                          "AND name NOT LIKE 'kv_%_conflicts' "
                                          "AND name NOT LIKE 'kv_%_seqs' "
                                          "AND sql NOT NULL");
        while(getIndex.executeStep()) {
            string indexName = getIndex.getColumn(0);
            string keyStoreName = getIndex.getColumn(1).getString().substr(3);
            if (!store || keyStoreName == store->name())
                indexes.emplace_back(indexName, IndexSpec::kValue, alloc_slice(),
                                     keyStoreName, "");
        }

        // FTS indexes:
        SQLite::Statement getFTS(*this, "SELECT name FROM sqlite_master WHERE type='table' "
                                        "AND name like '%::%' "
                                        "AND sql LIKE 'CREATE VIRTUAL TABLE % USING fts%'");
        while(getFTS.executeStep()) {
            string tableName = getFTS.getColumn(0).getString();
            auto delim = tableName.find("::");
            string keyStoreName = tableName.substr(delim);
            string indexName = tableName.substr(delim + 2);
            if (!store || keyStoreName == store->name())
                indexes.emplace_back(indexName, IndexSpec::kValue, alloc_slice(),
                                     keyStoreName, tableName);
        }
        return indexes;
    }


    // Gets info of a single index. (Subroutine of create/deleteIndex.)
    optional<SQLiteIndexSpec> SQLiteDataFile::getIndex(slice name) {
        if (!indexTableExists())
            return nullopt;
        SQLite::Statement stmt(*this, "SELECT name, type, expression, keyStore, indexTableName "
                                      "FROM indexes WHERE name=?");
        stmt.bindNoCopy(1, (char*)name.buf, (int)name.size);
        if (stmt.executeStep())
            return specFromStatement(stmt);
        else
            return nullopt;
    }


    SQLiteIndexSpec SQLiteDataFile::specFromStatement(SQLite::Statement &stmt) {
        alloc_slice expressionJSON;
        if (string col = stmt.getColumn(2).getString(); !col.empty())
            expressionJSON = col;
        return SQLiteIndexSpec(stmt.getColumn(0).getString(),
                               (IndexSpec::Type) stmt.getColumn(1).getInt(),
                               expressionJSON,
                               stmt.getColumn(3).getString(),
                               stmt.getColumn(4).getString());
    }


    void SQLiteDataFile::inspectIndex(slice name,
                                      int64_t &outRowCount,
                                      alloc_slice *outRows)
    {
        /* See  https://sqlite.org/imposter.html
           "Unlike all other SQLite APIs, sqlite3_test_control() interface is subject to incompatible
            changes from one release to the next, and so the mechanism described below is not
            guaranteed to work in future releases of SQLite. ...
            Imposter tables are for analysis and testing use only." */

        auto spec = getIndex(name);
        if (!spec)
            error::_throw(error::NoSuchIndex);
        else if (spec->type == IndexSpec::kFullText)
            error::_throw(error::UnsupportedOperation);

        // Construct a list of column names:
        stringstream columns;
        int n = 1;
        for (Array::iterator i(spec->what()); i; ++i, ++n) {
            auto col = i.value();
            if (auto array = col->asArray(); array)
                col = array->get(0);
            slice colStr = col->asString();
            if (colStr.hasPrefix("."_sl) && colStr.size > 1) {
                colStr.moveStart(1);
                columns << '"' << string(colStr) << '"';
            } else {
                columns << "c" << n;
            }
            columns << ", ";
        }
        columns << "_rowid";

        // Get the root page number of the index in the SQLite database file:
        int pageNo;
        {
            SQLite::Statement check(*_sqlDb, "SELECT sql, rootpage FROM sqlite_master "
                                    "WHERE type = 'index' AND name = ?");
            check.bind(1, spec->name);
            LogStatement(check);
            if (!check.executeStep())
                error::_throw(error::UnexpectedError, "Couldn't get internal index info");
            string sql = check.getColumn(0);
            pageNo = check.getColumn(1);
        }

        string tableName = "\"imposter::" + string(name) + "\"";

        sqlite3_test_control(SQLITE_TESTCTRL_IMPOSTER, _sqlDb->getHandle(), "main", 1, pageNo);
        _sqlDb->exec("CREATE TABLE IF NOT EXISTS " + tableName + " (" + columns.str()
                     + ", PRIMARY KEY(" + columns.str() + ")) WITHOUT ROWID");
        sqlite3_test_control(SQLITE_TESTCTRL_IMPOSTER, _sqlDb->getHandle(), "main", 0, 0);

        // Write the index's rows to a Fleece doc:
        if (outRows) {
            Encoder enc;
            enc.beginArray();
            SQLite::Statement st(*_sqlDb, "SELECT * FROM " + tableName);
            LogStatement(st);
            auto nCols = st.getColumnCount();
            outRowCount = 0;
            while (st.executeStep()) {
                ++outRowCount;
                enc.beginArray();
                for (int i = 0; i < nCols; ++i) {
                    SQLite::Column col = st.getColumn(i);
                    switch (col.getType()) {
                        case SQLITE_NULL:    enc << nullValue; break;
                        case SQLITE_INTEGER: enc << col.getInt(); break;
                        case SQLITE_FLOAT:   enc << col.getDouble(); break;
                        case SQLITE_TEXT:    enc << col.getString(); break;
                        case SQLITE_BLOB:    enc << "?BLOB?"; break; // TODO: Decode Fleece blobs
                    }
                }
                enc.endArray();
            }
            enc.endArray();
            *outRows = enc.finish();
        } else {
            outRowCount = this->intQuery(("SELECT count(*) FROM " + tableName).c_str());
        }
    }

}
