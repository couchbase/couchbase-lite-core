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
#include "Record.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "FleeceImpl.hh"
#include "Stopwatch.hh"
#include <sstream>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"
}

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    /*
     A value index is a SQL index named 'NAME'.
     A FTS index is a SQL virtual table named 'kv_default::NAME'
     An array index has two parts:
         * A SQL table named `kv_default:unnest:PATH`
         * An index on that table named `NAME`
     */

    static void validateIndexName(slice name);
    static pair<alloc_slice, const Array*> parseIndexExpr(slice expression, KeyStore::IndexType);
    static void writeTokenizerOptions(stringstream &sql, const KeyStore::IndexOptions*);


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
            case kFullTextIndex: created = createFTSIndex(indexNameStr, params, options); break;
            case kArrayIndex:    created = createArrayIndex(indexNameStr, params, options); break;
            default:             error::_throw(error::Unimplemented);
        }

        if (created) {
            garbageCollectArrayIndexes();
            t.commit();
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
        garbageCollectArrayIndexes();
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


#pragma mark - FTS INDEX:


    // Creates a FTS index.
    bool SQLiteKeyStore::createFTSIndex(string indexName,
                                        const Array *params,
                                        const IndexOptions *options)
    {
        auto ftsTableName = FTSTableName(indexName);
        // Collect the name of each FTS column and the SQL expression that populates it:
        QueryParser qp(*this);
        qp.setBodyColumnName("new.body");
        vector<string> colNames, colExprs;
        for (Array::iterator i(params); i; ++i) {
            colNames.push_back(CONCAT('"' << QueryParser::FTSColumnName(i.value()) << '"'));
            colExprs.push_back(qp.expressionSQL(i.value()));
        }
        string columns = join(colNames, ", ");
        string exprs = join(colExprs, ", ");

        // Build the SQL that creates an FTS table, including the tokenizer options:
        string sqlStr;
        {
            stringstream sql;
            sql << "CREATE VIRTUAL TABLE \"" << ftsTableName << "\" USING fts4(" << columns << ", ";
            writeTokenizerOptions(sql, options);
            sql << ")";
            sqlStr = sql.str();
        }

        // Create the FTS table, but if an identical one already exists, return:
        if (_schemaExistsWithSQL(ftsTableName, "table", ftsTableName, sqlStr))
            return false;
        _sqlDeleteIndex(indexName);
        LogTo(QueryLog, "Creating full-text search index '%s'", indexName.c_str());
        db().exec(sqlStr);

        // Index the existing records:
        db().exec(CONCAT("INSERT INTO \"" << ftsTableName << "\" (docid, " << columns << ") "
                         "SELECT rowid, " << exprs << " FROM kv_" << name() << " AS new"));

        // Set up triggers to keep the FTS table up to date
        // ...on insertion:
        createTrigger(ftsTableName, "ins", "AFTER INSERT", "",
                      CONCAT("INSERT INTO \"" << ftsTableName << "\" (docid, " << columns << ") "
                             "VALUES (new.rowid, " << exprs << ")"));

        // ...on delete:
        createTrigger(ftsTableName, "del", "AFTER DELETE", "",
                      CONCAT("DELETE FROM \"" << ftsTableName << "\" WHERE docid = old.rowid"));

        // ...on update:
        stringstream upd;
        upd << "UPDATE \"" << ftsTableName << "\" SET ";
        for (size_t i = 0; i < colNames.size(); ++i) {
            if (i > 0)
                upd << ", ";
            upd << colNames[i] << " = " << colExprs[i];
        }
        upd << " WHERE docid = new.rowid";
        createTrigger(ftsTableName, "upd", "AFTER UPDATE", "", upd.str());
        return true;
    }


    string SQLiteKeyStore::FTSTableName(const std::string &property) const {
        return tableName() + "::" + property;
    }


    // subroutine that generates the option string passed to the FTS tokenizer
    static void writeTokenizerOptions(stringstream &sql, const KeyStore::IndexOptions *options) {
        // See https://www.sqlite.org/fts3.html#tokenizer . 'unicodesn' is our custom tokenizer.
        sql << "tokenize=unicodesn";
        if (options) {
            // Get the language code (options->language might have a country too, like "en_US")
            string languageCode;
            if (options->language) {
                languageCode = options->language;
                auto u = languageCode.find('_');
                if (u != string::npos)
                    languageCode.resize(u);
            }
            if (options->stopWords) {
                string arg(options->stopWords);
                replace(arg, '"', ' ');
                replace(arg, ',', ' ');
                sql << " \"stopwordlist=" << arg << "\"";
            } else if (options->language) {
                sql << " \"stopwords=" << languageCode << "\"";
            }
            if (options->language && !options->disableStemming) {
                if (unicodesn_isSupportedStemmer(languageCode.c_str())) {
                    sql << " \"stemmer=" << languageCode << "\"";
                } else {
                    Warn("FTS does not support stemming for language code '%s'; ignoring it",
                         options->language);
                }
            }
            if (options->ignoreDiacritics) {
                sql << " \"remove_diacritics=1\"";
            }
        }
    }


#pragma mark - ARRAY INDEX:


    bool SQLiteKeyStore::createArrayIndex(string indexName,
                                          const Array *expressions,
                                          const IndexOptions *options)
    {
        Array::iterator iExprs(expressions);
        string arrayTableName = createUnnestedTable(iExprs.value(), options);
        return createValueIndex(kArrayIndex, arrayTableName, indexName, ++iExprs, options);
    }


    string SQLiteKeyStore::createUnnestedTable(const Value *path, const IndexOptions *options) {
        // Derive the table name from the expression (path) it unnests:
        auto kvTableName = tableName();
        auto unnestTableName = QueryParser(*this).unnestedTableName(path);

        // Create the index table, unless an identical one already exists:
        string sql = CONCAT("CREATE TABLE \"" << unnestTableName << "\" "
                            "(docid INTEGER NOT NULL REFERENCES " << kvTableName << "(rowid), "
                            " i INTEGER NOT NULL,"
                            " body BLOB NOT NULL, "
                            " CONSTRAINT pk PRIMARY KEY (docid, i)) "
                            "WITHOUT ROWID");
        if (!_schemaExistsWithSQL(unnestTableName, "table", unnestTableName, sql)) {
            LogTo(QueryLog, "Creating UNNEST table '%s'", unnestTableName.c_str());
            db().exec(sql);

            QueryParser qp(*this);
            qp.setBodyColumnName("new.body");
            string eachExpr = qp.eachExpressionSQL(path);

            // Populate the index-table with data from existing documents:
            db().exec(CONCAT("INSERT INTO \"" << unnestTableName << "\" (docid, i, body) "
                             "SELECT new.rowid, _each.rowid, _each.value " <<
                             "FROM " << kvTableName << " as new, " << eachExpr << " AS _each "
                             "WHERE (new.flags & 1) = 0"));

            // Set up triggers to keep the index-table up to date
            // ...on insertion:
            string insertTriggerExpr = CONCAT("INSERT INTO \"" << unnestTableName <<
                                              "\" (docid, i, body) "
                                              "SELECT new.rowid, _each.rowid, _each.value " <<
                                              "FROM " << eachExpr << " AS _each ");
            createTrigger(unnestTableName, "ins",
                          "AFTER INSERT",
                          "WHEN (new.flags & 1) = 0",
                          insertTriggerExpr);

            // ...on delete:
            string deleteTriggerExpr = CONCAT("DELETE FROM \"" << unnestTableName << "\" "
                                              "WHERE docid = old.rowid");
            createTrigger(unnestTableName, "del",
                          "BEFORE DELETE",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);

            // ...on update:
            createTrigger(unnestTableName, "preupdate",
                          "BEFORE UPDATE OF body, flags",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);
            createTrigger(unnestTableName, "postupdate",
                          "AFTER UPDATE OF body, flags",
                          "WHEN (new.flags & 1 = 0)",
                          insertTriggerExpr);
        }
        return unnestTableName;
    }


    string SQLiteKeyStore::unnestedTableName(const std::string &property) const {
        return tableName() + ":unnest:" + property;
    }


    // Drops unnested-array tables that no longer have any indexes on them.
    void SQLiteKeyStore::garbageCollectArrayIndexes() {
        vector<string> garbageTableNames;
        {
            SQLite::Statement st(db(),
                 "SELECT unnestTbl.name FROM sqlite_master as unnestTbl "
                  "WHERE unnestTbl.type='table' and unnestTbl.name like (?1 || ':unnest:%') "
                        "and not exists (SELECT * FROM sqlite_master "
                                         "WHERE type='index' and tbl_name=unnestTbl.name "
                                               "and sql not null)");
            st.bind(1, tableName());
            while(st.executeStep())
                garbageTableNames.push_back(st.getColumn(0));
        }
        for (string &tableName : garbageTableNames) {
            LogTo(QueryLog, "Dropping unused UNNEST table '%s'", tableName.c_str());
            db().exec(CONCAT("DROP TABLE \"" << tableName << "\""));
            dropTrigger(tableName, "ins");
            dropTrigger(tableName, "del");
            dropTrigger(tableName, "preupdate");
            dropTrigger(tableName, "postupdate");
        }
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
