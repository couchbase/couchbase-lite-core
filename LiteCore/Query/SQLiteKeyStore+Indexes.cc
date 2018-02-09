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
            case kValueIndex:    createValueIndex(indexNameStr, params, options); break;
            case kFullTextIndex: createFTSIndex(indexNameStr, params, options); break;
            default:             error::_throw(error::Unimplemented);
        }
        t.commit();
    }


    // Actually creates a value or FTS index, given the SQL statement to do so.
    // If an identical index with the same name exists, returns false.
    // Otherwise, any index with the same name is replaced.
    bool SQLiteKeyStore::_createIndex(IndexType type, const string &sqlName,
                                      const string &liteCoreName, const string &sql) {
        {
            SQLite::Statement check(db(), "SELECT sql FROM sqlite_master "
                                          "WHERE name = ? AND tbl_name = ? AND type = ?");
            check.bind(1, sqlName);
            check.bind(2, type == kValueIndex ? name()  : sqlName);
            check.bind(3, type == kValueIndex ? "index" : "table");
            if (check.executeStep() && check.getColumn(0).getString() == sql)
                return false;
        }
        _deleteIndex(liteCoreName);
        db().exec(sql, LogLevel::Info);
        return true;
    }


    // Creates a value index.
    void SQLiteKeyStore::createValueIndex(string indexName,
                                          const Array *params,
                                          const IndexOptions *options)
    {
        QueryParser qp(tableName());
        qp.writeCreateIndex(indexName, params);
        _createIndex(kValueIndex, indexName, indexName, qp.SQL());
    }


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
        stringstream sql;
        sql << "CREATE VIRTUAL TABLE \"" << ftsTableName << "\" USING fts4(" << columns << ", ";
        writeTokenizerOptions(sql, options);
        sql << ")";

        // Create the FTS table, but if an identical one already exists, return:
        if (!_createIndex(kFullTextIndex, ftsTableName, indexName, sql.str()))
            return;

        // Index the existing records:
        db().exec(CONCAT("INSERT INTO \"" << ftsTableName << "\" (docid, " << columns << ") "
                         "SELECT rowid, " << exprs << " FROM kv_" << name() << " AS new"));

        // Set up triggers to keep the FTS table up to date
        // ...on insertion:
        createTrigger(ftsTableName, "ins", "INSERT",
                      CONCAT("INSERT INTO \"" << ftsTableName << "\" (docid, " << columns << ") "
                             "VALUES (new.rowid, " << exprs << ")"));

        // ...on delete:
        createTrigger(ftsTableName, "del", "DELETE",
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
        createTrigger(ftsTableName, "upd", "UPDATE", upd.str());
    }


    void SQLiteKeyStore::_deleteIndex(slice name) {
        // Delete any expression index:
        validateIndexName(name);
        string indexName = (string)name;
        db().exec(CONCAT("DROP INDEX IF EXISTS \"" << indexName << "\""), LogLevel::Info);

        // Delete any FTS index:
        QueryParser qp(tableName());
        auto ftsTableName = qp.FTSTableName(indexName);
        db().exec(CONCAT("DROP TABLE IF EXISTS \"" << ftsTableName << "\""), LogLevel::Info);
        dropTrigger(ftsTableName, "ins");
        dropTrigger(ftsTableName, "upd");
        dropTrigger(ftsTableName, "del");
    }


    void SQLiteKeyStore::deleteIndex(slice name) {
        Transaction t(db());
        _deleteIndex(name);
        t.commit();
    }


    alloc_slice SQLiteKeyStore::getIndexes() const {
        Encoder enc;
        enc.beginArray();
        string tableNameStr = tableName();
        SQLite::Statement getIndex(db(), "SELECT name FROM sqlite_master WHERE type='index' "
                                            "AND tbl_name=? "
                                            "AND sql NOT NULL");
        getIndex.bind(1, tableNameStr);
        while(getIndex.executeStep()) {
            enc.writeString(getIndex.getColumn(0).getString());
        }

        SQLite::Statement getFTS(db(), "SELECT name FROM sqlite_master WHERE type='table' "
                                            "AND name like ? || '::%' "
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


    void SQLiteKeyStore::createSequenceIndex() {
        if (!_createdSeqIndex) {
            if (!_capabilities.sequences)
                error::_throw(error::NoSequences);
            db().execWithLock(CONCAT("CREATE UNIQUE INDEX IF NOT EXISTS kv_" << name() << "_seqs"
                                     " ON kv_" << name() << " (sequence)"));
            _createdSeqIndex = true;
        }
    }

}
