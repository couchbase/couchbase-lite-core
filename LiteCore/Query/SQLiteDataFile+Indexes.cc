//
// SQLiteDataFile+Indexes.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SQLiteDataFile.hh"
#include "SQLiteKeyStore.hh"
#include "SQLite_Internal.hh"
#include "Error.hh"
#include "Logging.hh"
#include "SecureDigest.hh"
#include "SQLiteCpp/Database.h"
#include "SQLUtil.hh"
#include "StringUtil.hh"
#include "Array.hh"
#include "Encoder.hh"
#include "sqlite3.h"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {


#pragma mark - INDEX-TABLE MANAGEMENT:

    bool SQLiteDataFile::indexTableExists() const {
        string sql;
        return getSchema("indexes", "table", "indexes", sql);
    }

    void SQLiteDataFile::ensureIndexTableExists() {
        if ( indexTableExists() ) return;

        if ( !options().upgradeable && _schemaVersion < SchemaVersion::WithIndexTable )
            error::_throw(error::CantUpgradeDatabase, "Accessing indexes requires upgrading the database schema");

        if ( !inTransaction() ) error::_throw(error::NotInTransaction);

        int userVersion = _sqlDb->execAndGet("PRAGMA user_version");
        if ( !options().upgradeable && userVersion < 301 )
            error::_throw(error::CantUpgradeDatabase, "Database needs upgrade of index metadata");

        LogTo(DBLog, "Upgrading database to use 'indexes' table...");
        _exec("CREATE TABLE indexes ("
              "name TEXT PRIMARY KEY, "   // Name of index
              "type INTEGER NOT NULL, "   // C4IndexType
              "keyStore TEXT NOT NULL, "  // Name of the KeyStore it indexes
              "expression TEXT, "         // Indexed property expression (JSON or N1QL)
              "whereClause TEXT, "        // Property whereClause for partial index (JSON or N1QL)
              "indexTableName TEXT, "     // Index's SQLite name
              "lastSeq TEXT)");           // indexed sequences, for lazy indexes, else null
        ensureSchemaVersionAtLeast(SchemaVersion::WithIndexTable);

        for ( auto& spec : getIndexesOldStyle() ) registerIndex(spec, spec.keyStoreName, spec.indexTableName);
    }

    void SQLiteDataFile::registerIndex(const litecore::IndexSpec& spec, const string& keyStoreName,
                                       const string& indexTableName) {
        SQLite::Statement stmt(*this,
                               "INSERT INTO indexes (name, type, keyStore, expression, indexTableName, whereClause) "
                               "VALUES (?, ?, ?, ?, ?, ?)");
        // CBL-6000 adding prefix to distinguish between JSON and N1QL expression
        string prefixedExpression{spec.queryLanguage == QueryLanguage::kJSON   ? "=j"
                                  : spec.queryLanguage == QueryLanguage::kN1QL ? "=n"
                                                                               : ""};
        // whereClause must be in the same queryLanguage as expression.

        prefixedExpression += spec.expression.asString();
        stmt.bindNoCopy(1, spec.name);
        stmt.bind(2, spec.type);
        stmt.bindNoCopy(3, keyStoreName);
        stmt.bindNoCopy(4, prefixedExpression.c_str(), (int)prefixedExpression.length());
        if ( spec.type != IndexSpec::kValue ) stmt.bindNoCopy(5, indexTableName);
        if ( !spec.whereClause.empty() ) stmt.bindNoCopy(6, (char*)spec.whereClause.buf, (int)spec.whereClause.size);

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

    bool SQLiteDataFile::createIndex(const litecore::IndexSpec& spec, SQLiteKeyStore* keyStore,
                                     const string& indexTableName, const string& indexSQL) {
        ensureIndexTableExists();
        if ( auto existingSpec = getIndex(spec.name) ) {
            if ( existingSpec->type == spec.type && existingSpec->keyStoreName == keyStore->name() ) {
                bool same;
                switch ( spec.type ) {
                    case IndexSpec::kFullText:
                    case IndexSpec::kVector:
                        same = schemaExistsWithSQL(indexTableName, "table", indexTableName, indexSQL);
                        if ( spec.type == IndexSpec::kFullText ) {
                            if ( same ) {
                                auto whatA = spec.what(), whatB = existingSpec->what();
                                same = whatA ? whatA->isEqual(whatB) : !whatB;
                            }
                            if ( same ) {
                                auto whereA = spec.where(), whereB = existingSpec->where();
                                same = whereA ? whereA->isEqual(whereB) : !whereB;
                            }
                        }
                        break;
                    case IndexSpec::kArray:
                        same = schemaExistsWithSQL(spec.name, "index", hexName(indexTableName), indexSQL);
                        break;
                    default:
                        same = schemaExistsWithSQL(spec.name, "index", indexTableName, indexSQL);
                        break;
                }
                if ( same ) return false;  // This is a duplicate of an existing index; do nothing
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

    void SQLiteDataFile::deleteIndex(const SQLiteIndexSpec& spec) {
        ensureIndexTableExists();
        LogTo(QueryLog, "Deleting %s index '%s'", spec.typeName(), spec.name.c_str());
        unregisterIndex(spec.name);
        if ( spec.type != IndexSpec::kFullText && spec.type != IndexSpec::kVector )
            exec(CONCAT("DROP INDEX IF EXISTS " << sqlIdentifier(spec.name)));
        if ( !spec.indexTableName.empty() ) garbageCollectIndexTable(spec);
    }

    // Drops unnested-array tables that no longer have any indexes on them.
    void SQLiteDataFile::garbageCollectIndexTable(const SQLiteIndexSpec& spec) {
        string              tableName = spec.indexTableName;  // tableName is in plain form.
        std::vector<string> unnestTables;
        if ( spec.type == IndexSpec::kArray ) {
            for ( size_t pos = 0; pos != string::npos; ) {
                pos = tableName.find(KeyStore::kUnnestLevelSeparator, pos);
                if ( pos != string::npos ) {
                    unnestTables.push_back(tableName.substr(0, pos));
                    pos += KeyStore::kUnnestLevelSeparator.size;
                } else {
                    unnestTables.push_back(tableName);
                }
            }
            DebugAssert(unnestTables.back() == tableName);
        }

        size_t unnestLevel = unnestTables.size();
        // Invariant: spec.type != IndexSpec::kArray || unnestLevel >= 1
        while ( true ) {
            {
                // Check that if the index table still exists. If so, don't do GC
                SQLite::Statement stmt(*this, "SELECT name FROM indexes WHERE indexTableName=?");
                stmt.bind(1, tableName);
                if ( stmt.executeStep() ) return;
            }
            if ( spec.type == IndexSpec::kArray ) {
                // Assertion: unnestLevel >= 1
                // check if it has index on child/nested array. If so, we cannot GC
                SQLite::Statement stmt(*this, "SELECT name FROM indexes WHERE indexTableName like ?");
                stmt.bind(1, unnestTables[unnestLevel - 1] + string(KeyStore::kUnnestLevelSeparator) + "%");
                // This table has child table. Don't delete it and its parent
                if ( stmt.executeStep() ) return;

                tableName = hexName(tableName);  // Now, it's the true table name, hashed.
                // Move on to delete the unused tables.
            }

            LogTo(QueryLog, "Dropping unused index table '%s'", tableName.c_str());
            exec(CONCAT("DROP TABLE " << sqlIdentifier(tableName) << ""));

            static const char* kTriggerSuffixes[]       = {"ins", "del", "upd", "preupdate", "postupdate", nullptr};
            static const char* kNestedTriggerSuffixes[] = {"ins", "del", nullptr};

            const char** triggerSuffixes = kTriggerSuffixes;
            if ( unnestLevel > 1 ) triggerSuffixes = kNestedTriggerSuffixes;

            {
                stringstream sql;
                for ( int i = 0; triggerSuffixes[i]; ++i ) {
                    sql << "DROP TRIGGER IF EXISTS " << sqlIdentifier(tableName + "::" + triggerSuffixes[i]) << ";";
                }
                exec(sql.str());
            }

            if ( unnestLevel-- <= 1 ) return;
            else
                tableName = unnestTables[unnestLevel - 1];
        }
    }

#pragma mark - GETTING INDEX INFO:

    vector<SQLiteIndexSpec> SQLiteDataFile::getIndexes(const KeyStore* store) const {
        if ( indexTableExists() ) {
            string sql = "SELECT name, type, expression, keyStore, indexTableName, lastSeq, whereClause "
                         "FROM indexes ORDER BY name";
            if ( _schemaVersion < SchemaVersion::WithIndexesLastSeq ) {
                // If schema doesn't have the `lastSeq` column, don't query it:
                replace(sql, "lastSeq", "NULL");
            }
            vector<SQLiteIndexSpec> indexes;
            SQLite::Statement       stmt(*this, sql);
            while ( stmt.executeStep() ) {
                string keyStoreName = stmt.getColumn(3);
                if ( !store || keyStoreName == store->name() ) indexes.emplace_back(specFromStatement(stmt));
            }
            return indexes;
        } else {
            return getIndexesOldStyle(store);
        }
    }

    // Finds the indexes the old 2.0/2.1 way, without using the 'indexes' table.
    vector<SQLiteIndexSpec> SQLiteDataFile::getIndexesOldStyle(const KeyStore* store) const {
        vector<SQLiteIndexSpec> indexes;
        // value indexes:
        SQLite::Statement getIndex(*this, "SELECT name, tbl_name FROM sqlite_master "
                                          "WHERE type = 'index' "
                                          "AND tbl_name LIKE 'kv_%' "
                                          "AND name NOT LIKE 'kv_%_blobs' "
                                          "AND name NOT LIKE 'kv_%_conflicts' "
                                          "AND name NOT LIKE 'kv_%_seqs' "
                                          "AND name NOT LIKE 'kv_%_expiration' "
                                          "AND sql NOT NULL");
        while ( getIndex.executeStep() ) {
            string indexName    = getIndex.getColumn(0);
            string keyStoreName = getIndex.getColumn(1).getString().substr(3);
            if ( !store || keyStoreName == store->name() )
                indexes.emplace_back(indexName, IndexSpec::kValue, alloc_slice(), QueryLanguage::kJSON,
                                     IndexSpec::Options{}, keyStoreName, "");
        }

        // FTS indexes:
        SQLite::Statement getFTS(*this, "SELECT name FROM sqlite_master WHERE type='table' "
                                        "AND name like '%::%' "
                                        "AND sql LIKE 'CREATE VIRTUAL TABLE % USING fts%'");
        while ( getFTS.executeStep() ) {
            string tableName    = getFTS.getColumn(0).getString();
            auto   delim        = tableName.find("::");
            string keyStoreName = tableName.substr(delim);
            string indexName    = tableName.substr(delim + 2);
            if ( !store || keyStoreName == store->name() )
                indexes.emplace_back(indexName, IndexSpec::kValue, alloc_slice(), QueryLanguage::kJSON,
                                     IndexSpec::Options{}, keyStoreName, tableName);
        }
        return indexes;
    }

    // Gets info of a single index. (Subroutine of create/deleteIndex.)
    optional<SQLiteIndexSpec> SQLiteDataFile::getIndex(slice name) {
        if ( !indexTableExists() ) return nullopt;
        string sql = "SELECT name, type, expression, keyStore, indexTableName, lastSeq, whereClause "
                     "FROM indexes WHERE name=?";
        if ( _schemaVersion < SchemaVersion::WithIndexesLastSeq ) {
            // If schema doesn't have the `lastSeq` column, don't query it:
            replace(sql, "lastSeq", "NULL");
        }
        SQLite::Statement stmt(*this, sql);
        stmt.bindNoCopy(1, (char*)name.buf, (int)name.size);
        if ( stmt.executeStep() ) return specFromStatement(stmt);
        else
            return nullopt;
    }

    void SQLiteDataFile::setIndexSequences(slice name, slice sequencesJSON) {
        if ( _schemaVersion < SchemaVersion::WithIndexesLastSeq )
            error::_throw(error::CantUpgradeDatabase, "Saving lazy index-state requires updating database schema");
        SQLite::Statement stmt(*this, "UPDATE indexes SET lastSeq=?1 WHERE name=?2");
        stmt.bindNoCopy(1, (char*)sequencesJSON.buf, int(sequencesJSON.size));
        stmt.bindNoCopy(2, (char*)name.buf, (int)name.size);
        stmt.exec();
    }

    // Recover an IndexSpec from a row of the `indexes` table
    SQLiteIndexSpec SQLiteDataFile::specFromStatement(SQLite::Statement& stmt) const {
        string             name = stmt.getColumn(0).getString();
        auto               type = IndexSpec::Type(stmt.getColumn(1).getInt());
        IndexSpec::Options options;
        string             keyStoreName   = stmt.getColumn(3).getString();
        string             indexTableName = stmt.getColumn(4).getString();

        QueryLanguage queryLanguage = QueryLanguage::kJSON;
        alloc_slice   expression;
        if ( string col = stmt.getColumn(2).getString(); !col.empty() ) {
            if ( col[0] == '=' ) {
                // This is new after cbl-6000. c.f. SQLiteDataFile::registerIndex
                if ( col[1] == 'j' ) queryLanguage = QueryLanguage::kJSON;
                else if ( col[1] == 'n' )
                    queryLanguage = QueryLanguage::kN1QL;
                else
                    error::_throw(error::UnexpectedError, "Expression in the index table has unexpected prefix.");
                expression = col.substr(2);
            } else {
                // Old style, without prefix.
                expression = col;
                if ( col[0] != '[' && col[0] != '{' ) queryLanguage = QueryLanguage::kN1QL;
            }
        }

#ifdef COUCHBASE_ENTERPRISE
        if ( type == IndexSpec::kVector ) {
            // Recover the vector options from the index schema itself:
            string sql;
            if ( getSchema(indexTableName, "table", indexTableName, sql) ) {
                if ( auto opts = SQLiteKeyStore::parseVectorSearchTableSQL(sql) ) options = std::move(*opts);
            }
        }
#endif

        if ( type == IndexSpec::kArray ) {
            auto        pos = indexTableName.find(KeyStore::kUnnestSeparator);
            string_view path{""};
            if ( pos != string::npos )
                path = string_view{indexTableName.data() + pos + KeyStore::kUnnestSeparator.size,
                                   indexTableName.length() - pos - KeyStore::kUnnestSeparator.size};
            options.emplace<IndexSpec::ArrayOptions>(IndexSpec::ArrayOptions{path});
        }

        SQLiteIndexSpec spec{name, type, expression, queryLanguage, options, keyStoreName, indexTableName};
        if ( auto col5 = stmt.getColumn(5); col5.isText() ) spec.indexedSequences = col5.getText();
        if ( auto col6 = stmt.getColumn(6); col6.isText() ) spec.setWhereClause({col6.getText()});
        return spec;
    }

    optional<SQLiteIndexSpec> SQLiteDataFile::findIndexOnExpression(const string& jsonWhat, IndexSpec::Type type,
                                                                    const string& onTable) const {
        for ( SQLiteIndexSpec& spec : getIndexes(nullptr) ) {
            if ( spec.type == type && SQLiteKeyStore::tableName(spec.keyStoreName) == onTable ) {
                auto what = spec.what();
                // `what()` is defined as an array of 1+ exprs to index; for a vector index there can be only one.
                // In some cases just that term is passed in, not wrapped in an array.
                if ( what->count() > 1
                     || (spec.queryLanguage == QueryLanguage::kN1QL || what->get(0)->type() == kArray) ) {
                    what = (const Array*)what->get(0);
                }
                if ( what->toJSON(true) == jsonWhat ) return std::move(spec);
            }
        }
        return nullopt;
    }

#pragma mark - FOR DEBUGGING / INSPECTION:

    void SQLiteDataFile::inspectIndex(slice name, int64_t& outRowCount, alloc_slice* outRows) {
        /* See  https://sqlite.org/imposter.html
           "Unlike all other SQLite APIs, sqlite3_test_control() interface is subject to incompatible
            changes from one release to the next, and so the mechanism described below is not
            guaranteed to work in future releases of SQLite. ...
            Imposter tables are for analysis and testing use only." */

        auto spec = getIndex(name);
        if ( !spec ) error::_throw(error::NoSuchIndex);
        else if ( spec->type == IndexSpec::kVector )
            return inspectVectorIndex(*spec, outRowCount, outRows);
        else if ( spec->type != IndexSpec::kValue )
            error::_throw(error::UnsupportedOperation, "Only supported for value indexes");

        // Construct a list of column names:
        stringstream columns;
        int          n = 1;
        for ( Array::iterator i(spec->what()); i; ++i, ++n ) {
            auto col = i.value();
            if ( auto array = col->asArray(); array ) col = array->get(0);
            slice colStr = col->asString();
            if ( colStr.hasPrefix("."_sl) && colStr.size > 1 ) {
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
            if ( !check.executeStep() ) error::_throw(error::UnexpectedError, "Couldn't get internal index info");
            string sql = check.getColumn(0);
            pageNo     = check.getColumn(1);
        }

        string tableName = "\"imposter::" + string(name) + "\"";

        sqlite3_test_control(SQLITE_TESTCTRL_IMPOSTER, _sqlDb->getHandle(), "main", 1, pageNo);
        _sqlDb->exec("CREATE TABLE IF NOT EXISTS " + tableName + " (" + columns.str() + ", PRIMARY KEY(" + columns.str()
                     + ")) WITHOUT ROWID");
        sqlite3_test_control(SQLITE_TESTCTRL_IMPOSTER, _sqlDb->getHandle(), "main", 0, 0);

        // Write the index's rows to a Fleece doc:
        if ( outRows ) {
            Encoder enc;
            enc.beginArray();
            SQLite::Statement st(*_sqlDb, "SELECT * FROM " + tableName);
            LogStatement(st);
            auto nCols  = st.getColumnCount();
            outRowCount = 0;
            while ( st.executeStep() ) {
                ++outRowCount;
                enc.beginArray();
                for ( int i = 0; i < nCols; ++i ) {
                    SQLite::Column col = st.getColumn(i);
                    switch ( col.getType() ) {
                        case SQLITE_NULL:
                            enc << nullValue;
                            break;
                        case SQLITE_INTEGER:
                            enc << col.getInt();
                            break;
                        case SQLITE_FLOAT:
                            enc << col.getDouble();
                            break;
                        case SQLITE_TEXT:
                            enc << col.getString();
                            break;
                        case SQLITE_BLOB:
                            enc << "?BLOB?";
                            break;  // TODO: Decode Fleece blobs
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

    void SQLiteDataFile::inspectVectorIndex(SQLiteIndexSpec const& spec, int64_t& outRowCount, alloc_slice* outRows) {
        if ( outRows ) {
            string            ksTable = SQLiteKeyStore::tableName(spec.keyStoreName);
            SQLite::Statement st(*_sqlDb, "SELECT kv.key, idx.vector, idx.bucket, idx.docid"
                                          " FROM \""
                                                  + spec.indexTableName
                                                  + "\" as idx"
                                                    " LEFT JOIN \""
                                                  + ksTable
                                                  + "\" as kv ON idx.docid = kv.rowid"
                                                    " ORDER BY kv.key");
            LogStatement(st);
            Encoder enc;
            enc.beginArray();
            outRowCount = 0;
            while ( st.executeStep() ) {
                ++outRowCount;
                enc.beginArray();
                enc.writeString(st.getColumn(0).getText());
                enc.writeData(slice(st.getColumn(1).getBlob(), st.getColumn(1).size()));
                enc.writeInt(st.getColumn(2));
                enc.writeInt(st.getColumn(3));
                enc.endArray();
            }
            enc.endArray();
            *outRows = enc.finish();
        } else {
            outRowCount = this->intQuery(("SELECT count(*) FROM " + spec.indexTableName).c_str());
        }
    }

}  // namespace litecore
