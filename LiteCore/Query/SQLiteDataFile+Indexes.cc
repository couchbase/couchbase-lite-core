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
#include "SQLiteCpp/SQLiteCpp.h"

using namespace std;

namespace litecore {


#pragma mark - INDEX-TABLE MANAGEMENT:


    bool SQLiteDataFile::indexTableExists() {
        string sql;
        return getSchema("indexes", "table", "indexes", sql);
    }


    void SQLiteDataFile::ensureIndexTableExists() {
        if (indexTableExists())
            return;

        Assert(inTransaction());
        LogTo(DBLog, "Upgrading database to use 'indexes' table...");
        _exec("CREATE TABLE indexes (name TEXT PRIMARY KEY, type INTEGER NOT NULL,"
                                  " keyStore TEXT NOT NULL, expression TEXT, indexTableName TEXT)");
        ensureUserVersionAtLeast(301); // Backward-incompatible with version 2.0/2.1

        for (auto &spec : getIndexesOldStyle())
            registerIndex(spec, spec.keyStoreName, spec.indexTableName);
    }


    void SQLiteDataFile::registerIndex(const KeyStore::IndexSpec &spec,
                                       const string &keyStoreName, const string &indexTableName)
    {
        SQLite::Statement stmt(*this, "INSERT INTO indexes (name, type, keyStore, expression, indexTableName) "
                                      "VALUES (?, ?, ?, ?, ?)");
        stmt.bindNoCopy(1, spec.name);
        stmt.bind(      2, spec.type);
        stmt.bindNoCopy(3, keyStoreName);
        stmt.bindNoCopy(4, (char*)spec.expressionJSON.buf, (int)spec.expressionJSON.size);
        if (spec.type != KeyStore::kValueIndex)
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


    bool SQLiteDataFile::createIndex(const KeyStore::IndexSpec &spec,
                                     SQLiteKeyStore *keyStore,
                                     const string &indexTableName,
                                     const string &indexSQL)
    {
        ensureIndexTableExists();
        auto existingSpec = getIndex(spec.name);
        if (existingSpec) {
            if (existingSpec.type == spec.type && existingSpec.keyStoreName == keyStore->name()) {
                bool same;
                if (spec.type == KeyStore::kFullTextIndex)
                    same = schemaExistsWithSQL(indexTableName, "table", indexTableName, indexSQL);
                else
                    same = schemaExistsWithSQL(spec.name, "index", indexTableName, indexSQL);
                if (same)
                    return false;       // This is a duplicate of an existing index; do nothing
            }
            // Existing index is different, so delete it first:
            deleteIndex(existingSpec);
        }
        LogTo(QueryLog, "Creating %s index \"%s\"",
              KeyStore::kIndexTypeName[spec.type], spec.name.c_str());
        exec(indexSQL);
        registerIndex(spec, keyStore->name(), indexTableName);
        return true;
    }


#pragma mark - DELETING INDEXES:


    void SQLiteDataFile::deleteIndex(const IndexSpec &spec) {
        ensureIndexTableExists();
        LogTo(QueryLog, "Deleting %s index '%s'",
              KeyStore::kIndexTypeName[spec.type], spec.name.c_str());
        unregisterIndex(spec.name);
        if (spec.type != KeyStore::kFullTextIndex)
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


    vector<SQLiteDataFile::IndexSpec> SQLiteDataFile::getIndexes(const KeyStore *store) {
        if (indexTableExists()) {
            vector<IndexSpec> indexes;
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
    vector<SQLiteDataFile::IndexSpec> SQLiteDataFile::getIndexesOldStyle(const KeyStore *store) {
        vector<IndexSpec> indexes;
        // value indexes:
        SQLite::Statement getIndex(*this, "SELECT name, tbl_name FROM sqlite_master "
                                          "WHERE type = 'index' "
                                          "AND tbl_name LIKE 'kv_%' "
                                          "AND name NOT LIKE 'kv_%_seqs' "
                                          "AND sql NOT NULL");
        while(getIndex.executeStep()) {
            string indexName = getIndex.getColumn(0);
            string keyStoreName = getIndex.getColumn(1).getString().substr(3);
            if (!store || keyStoreName == store->name())
                indexes.emplace_back(indexName, KeyStore::kValueIndex, alloc_slice(),
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
                indexes.emplace_back(indexName, KeyStore::kValueIndex, alloc_slice(),
                                     keyStoreName, tableName);
        }
        return indexes;
    }


    // Gets info of a single index. (Subroutine of create/deleteIndex.)
    SQLiteDataFile::IndexSpec SQLiteDataFile::getIndex(slice name) {
        ensureIndexTableExists();
        SQLite::Statement stmt(*this, "SELECT name, type, expression, keyStore, indexTableName "
                                      "FROM indexes WHERE name=?");
        stmt.bindNoCopy(1, (char*)name.buf, (int)name.size);
        if (stmt.executeStep())
            return specFromStatement(stmt);
        else
            return {};
    }


    SQLiteDataFile::IndexSpec SQLiteDataFile::specFromStatement(SQLite::Statement &stmt) {
        IndexSpec spec(stmt.getColumn(0).getString(),
                       (KeyStore::IndexType) stmt.getColumn(1).getInt(),
                       alloc_slice(stmt.getColumn(2).getString()),
                       stmt.getColumn(3).getString(),
                       stmt.getColumn(4).getString());
        if (spec.expressionJSON.size == 0)
            spec.expressionJSON = nullslice;
        return spec;
    }


}
