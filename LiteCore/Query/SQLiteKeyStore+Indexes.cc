//
// SQLiteKeyStore+Indexes.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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


#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Query.hh"
#include "QueryParser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "Stopwatch.hh"

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    /*
     - A value index is a SQL index named 'NAME'.
     - A FTS index is a SQL virtual table named 'kv_default::NAME'
     - An array index has two parts:
         * A SQL table named `kv_default:unnest:PATH`, where PATH is the property path
         * An index on that table named `NAME`
     - A predictive index has two parts:
         * A SQL table named `kv_default:prediction:DIGEST`, where DIGEST is a unique digest
            of the prediction function name and the parameter dictionary
         * An index on that table named `NAME`
     */

    static void validateIndexName(slice name);
    static pair<alloc_slice, const Array*> parseIndexExpr(slice expression, KeyStore::IndexType);


    bool SQLiteKeyStore::createIndex(slice indexName,
                                     slice expression,
                                     IndexType type,
                                     const IndexOptions *options) {
        validateIndexName(indexName);
        auto indexNameStr = string(indexName);
        alloc_slice expressionFleece;
        const Array *params;
        tie(expressionFleece, params) = parseIndexExpr(expression, type);

        Stopwatch st;
        Transaction t(db());
        bool created;
        switch (type) {
            case kValueIndex: {
                Array::iterator iParams(params);
                created = createValueIndex(kValueIndex, tableName(), indexNameStr, iParams, options);
                break;
            }
            case kFullTextIndex:  created = createFTSIndex(indexNameStr, params, options); break;
            case kArrayIndex:     created = createArrayIndex(indexNameStr, params, options); break;
#ifdef COUCHBASE_ENTERPRISE
            case kPredictiveIndex:created = createPredictiveIndex(indexNameStr, params, options); break;
#endif
            default:             error::_throw(error::Unimplemented);
        }

        if (created) {
            garbageCollectIndexTables();
            t.commit();
            db().optimize();
            double time = st.elapsed();
            QueryLog.log((time < 3.0 ? LogLevel::Info : LogLevel::Warning),
                         "Created index '%.*s' in %.3f sec", SPLAT(indexName), time);
        }
        return created;
    }


    void SQLiteKeyStore::deleteIndex(slice name) {
        validateIndexName(name);
        Transaction t(db());
        LogTo(QueryLog, "Deleting index '%.*s'", SPLAT(name));
        _sqlDeleteIndex(string(name));
        garbageCollectIndexTables();
        t.commit();
    }


    // Creates the special by-sequence index
    void SQLiteKeyStore::createSequenceIndex() {
        if (!_createdSeqIndex) {
            Assert(_capabilities.sequences);
            db().execWithLock(CONCAT("CREATE UNIQUE INDEX IF NOT EXISTS kv_" << name() << "_seqs"
                                     " ON kv_" << name() << " (sequence)"));
            _createdSeqIndex = true;
        }
    }


    alloc_slice SQLiteKeyStore::getIndexes() const {
        Encoder enc;
        enc.beginArray();
        string tableNameStr = tableName();

        // First find indexes on this KeyStore, or on one of its unnested tables:
        SQLite::Statement getIndex(db(), "SELECT name FROM sqlite_master WHERE type='index' "
                                         "AND (tbl_name=?1 OR tbl_name like (?1 || ':unnest:%')) "
                                         "AND sql NOT NULL");
        getIndex.bind(1, tableNameStr);
        while(getIndex.executeStep()) {
            enc.writeString(getIndex.getColumn(0).getString());
        }

        // Now find FTS tables on this KeyStore:
        SQLite::Statement getFTS(db(), "SELECT name FROM sqlite_master WHERE type='table' "
                                       "AND name like (? || '::%') "
                                       "AND sql LIKE 'CREATE VIRTUAL TABLE % USING fts%'");
        getFTS.bind(1, tableNameStr);
        while(getFTS.executeStep()) {
            string ftsName = getFTS.getColumn(0).getString();
            ftsName = ftsName.substr(ftsName.find("::") + 2);
            enc.writeString(ftsName);
        }

        enc.endArray();
        return enc.finish();
    }


    // Actually deletes an index from SQLite.
    void SQLiteKeyStore::_sqlDeleteIndex(const string &indexName) {
        // Delete any expression or array index:
        db().exec(CONCAT("DROP INDEX IF EXISTS \"" << indexName << "\""));

        // Delete any FTS index:
        auto ftsTableName = FTSTableName(indexName);
        db().exec(CONCAT("DROP TABLE IF EXISTS \"" << ftsTableName << "\""));
        dropTrigger(ftsTableName, "ins");
        dropTrigger(ftsTableName, "upd");
        dropTrigger(ftsTableName, "del");
    }


#pragma mark - VALUE INDEX:


    // Creates a value index.
    bool SQLiteKeyStore::createValueIndex(IndexType type,
                                          const string &sourceTableName,
                                          const string &indexName,
                                          Array::iterator &expressions,
                                          const IndexOptions *options)
    {
        QueryParser qp(*this);
        qp.setTableName(CONCAT('"' << sourceTableName << '"'));
        qp.writeCreateIndex(indexName, expressions, (type == kArrayIndex));
        string sql = qp.SQL();
        if (_schemaExistsWithSQL(indexName, "index", sourceTableName, sql))
            return false;
        _sqlDeleteIndex(indexName);
        LogTo(QueryLog, "Creating %sindex '%s'",
              (type == kArrayIndex ? "array " : ""), indexName.c_str());
        db().exec(sql);
        return true;
    }


#pragma mark - UTILITIES:


    // Part of the QueryParser delegate API
    bool SQLiteKeyStore::tableExists(const std::string &tableName) const {
        return db().tableExists(tableName);
    }


    // Returns true if an index/table exists in the database with the given type and SQL schema
    bool SQLiteKeyStore::_schemaExistsWithSQL(const string &name, const string &type,
                                              const string &tableName, const string &sql) {
        string existingSQL;
        return db().getSchema(name, type, tableName, existingSQL) && existingSQL == sql;
    }


    static void validateIndexName(slice name) {
        if(name.size == 0) {
            error::_throw(error::LiteCoreError::InvalidParameter, "Index name must not be empty");
        }

        if(name.findByte((uint8_t)'"') != nullptr) {
            error::_throw(error::LiteCoreError::InvalidParameter, "Index name must not contain "
                          "the double quote (\") character");
        }
    }


    // Parses the JSON index-spec expression into an Array:
    static pair<alloc_slice, const Array*> parseIndexExpr(slice expression,
                                                          KeyStore::IndexType type)
    {
        alloc_slice expressionFleece;
        const Array *params = nullptr;
        try {
            Retained<Doc> doc = Doc::fromJSON(expression);
            expressionFleece = doc->allocedData();
            params = doc->asArray();
        } catch (const FleeceException &) { }
        if (!params || params->count() == 0)
            error::_throw(error::InvalidQuery);
        return {expressionFleece, params};
    }

}
