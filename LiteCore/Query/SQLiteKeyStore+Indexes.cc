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
#include "QueryParser.hh"
#include "Record.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "Fleece.hh"
#include <sstream>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"
}

using namespace std;
using namespace fleece;

namespace litecore {

    /*
     A value index is a SQL index named 'NAME'.
     A FTS index is a SQL virtual table named 'kv_default::NAME'
     An array index has two parts:
         * A SQL table named `kv_default::unnest::PATH`
         * An index on that table named `NAME`
     */

    static void validateIndexName(slice name);
    static pair<alloc_slice, const Array*> parseIndexExpr(slice expression, KeyStore::IndexType);
    static void writeTokenizerOptions(stringstream &sql, const KeyStore::IndexOptions*);


    void SQLiteKeyStore::createIndex(slice indexName,
                                     slice expression,
                                     IndexType type,
                                     const IndexOptions *options) {
        validateIndexName(indexName);
        auto indexNameStr = string(indexName);
        alloc_slice expressionFleece;
        const Array *params;
        tie(expressionFleece, params) = parseIndexExpr(expression, type);

        Transaction t(db());
        switch (type) {
            case kValueIndex: {
                Array::iterator iParams(params);
                createValueIndex(tableName(), indexNameStr, iParams, options);
                break;
            }
            case kFullTextIndex: createFTSIndex(indexNameStr, params, options); break;
            case kArrayIndex:    createArrayIndex(indexNameStr, params, options); break;
            default:             error::_throw(error::Unimplemented);
        }
        t.commit();
    }


    void SQLiteKeyStore::deleteIndex(slice name) {
        Transaction t(db());
        _sqlDeleteIndex(name);
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
        return enc.extractOutput();
    }


    // Returns true if an entity exists in the database with the given type and SQL schema
    bool SQLiteKeyStore::_sqlExists(const string &name, const string &type,
                                    const string &tableName, const string &sql) {
        string existingSQL;
        return db().getSchema(name, type, tableName, existingSQL) && existingSQL == sql;
    }


    // Actually deletes an index from SQLite.
    void SQLiteKeyStore::_sqlDeleteIndex(slice name) {
        // Delete any expression or array index:
        validateIndexName(name);
        string indexName = (string)name;
        db().exec(CONCAT("DROP INDEX IF EXISTS \"" << indexName << "\""));

        //TODO: Garbage-collect any unnest table that has no more array indexes

        // Delete any FTS index:
        QueryParser qp(tableName());
        auto ftsTableName = qp.FTSTableName(indexName);
        db().exec(CONCAT("DROP TABLE IF EXISTS \"" << ftsTableName << "\""));
        dropTrigger(ftsTableName, "ins");
        dropTrigger(ftsTableName, "upd");
        dropTrigger(ftsTableName, "del");
    }


#pragma mark - VALUE INDEX:


    // Creates a value index.
    void SQLiteKeyStore::createValueIndex(const string &sourceTableName,
                                          const string &indexName,
                                          Array::iterator &expressions,
                                          const IndexOptions *options)
    {
        QueryParser qp(CONCAT('"' << sourceTableName << '"'));
        qp.writeCreateIndex(indexName, expressions);
        string sql = qp.SQL();
        if (_sqlExists(indexName, "index", sourceTableName, sql))
            return;
        _sqlDeleteIndex(indexName);
        db().exec(sql);
    }


#pragma mark - FTS INDEX:


    // Creates a FTS index.
    void SQLiteKeyStore::createFTSIndex(string indexName,
                                        const Array *params,
                                        const IndexOptions *options)
    {
        auto ftsTableName = QueryParser(tableName()).FTSTableName(indexName);
        // Collect the name of each FTS column and the SQL expression that populates it:
        vector<string> colNames, colExprs;
        for (Array::iterator i(params); i; ++i) {
            colNames.push_back(CONCAT('"' << QueryParser::FTSColumnName(i.value()) << '"'));
            colExprs.push_back(QueryParser::expressionSQL(i.value(), "new.body"));
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
        if (_sqlExists(ftsTableName, "table", ftsTableName, sqlStr))
            return;
        _sqlDeleteIndex(indexName);
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


    void SQLiteKeyStore::createArrayIndex(string indexName,
                                          const Array *expressions,
                                          const IndexOptions *options)
    {
        Array::iterator iExprs(expressions);
        string arrayTableName = createUnnestedTable(iExprs.value(), options);
        createValueIndex(arrayTableName, indexName, ++iExprs, options);
    }


    string SQLiteKeyStore::createUnnestedTable(const Value *path, const IndexOptions *options) {
        // Derive the table name from the expression (path) it unnests:
        auto kvTableName = tableName();
        auto arrayTableName = QueryParser(kvTableName).unnestedTableName(path);

        // Create the index table, unless an identical one already exists:
        string sql = CONCAT("CREATE TABLE \"" << arrayTableName << "\" "
                            "(docid INTEGER NOT NULL REFERENCES " << kvTableName << "(rowid), "
                            " i INTEGER NOT NULL, body BLOB NOT NULL, "
                            " CONSTRAINT pk PRIMARY KEY (docid, i)) "
                            "WITHOUT ROWID");
        if (!_sqlExists(arrayTableName, "table", arrayTableName, sql)) {
            db().exec(sql);

            string eachExpr = QueryParser::eachExpressionSQL(path, "new.body");

            // Populate the index-table with data from existing documents:
            db().exec(CONCAT("INSERT INTO \"" << arrayTableName << "\" (docid, i, body) "
                             "SELECT new.rowid, _each.rowid, _each.value " <<
                             "FROM " << kvTableName << " as new, " << eachExpr << " AS _each "
                             "WHERE (new.flags & 1) = 0"));

            // Set up triggers to keep the index-table up to date
            // ...on insertion:
            string insertTriggerExpr = CONCAT("INSERT INTO \"" << arrayTableName <<
                                              "\" (docid, i, body) "
                                              "SELECT new.rowid, _each.rowid, _each.value " <<
                                              "FROM " << eachExpr << " AS _each ");
            createTrigger(arrayTableName, "ins",
                          "AFTER INSERT",
                          "WHEN (new.flags & 1) = 0",
                          insertTriggerExpr);

            // ...on delete:
            string deleteTriggerExpr = CONCAT("DELETE FROM \"" << arrayTableName << "\" "
                                              "WHERE docid = old.rowid");
            createTrigger(arrayTableName, "del",
                          "BEFORE DELETE",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);

            // ...on update:
            createTrigger(arrayTableName, "preupdate",
                          "BEFORE UPDATE OF body, flags",
                          "WHEN (old.flags & 1) = 0",
                          deleteTriggerExpr);
            createTrigger(arrayTableName, "postupdate",
                          "AFTER UPDATE OF body, flags",
                          "WHEN (new.flags & 1 = 0)",
                          insertTriggerExpr);
        }
        return arrayTableName;
    }



#pragma mark - UTILITIES:


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
            expressionFleece = JSONConverter::convertJSON(expression);
            auto f = Value::fromTrustedData(expressionFleece);
            if (f)
                params = f->asArray();
        } catch (const FleeceException &) { }
        if (!params || params->count() == 0)
            error::_throw(error::InvalidQuery);
        return {expressionFleece, params};
    }

}
