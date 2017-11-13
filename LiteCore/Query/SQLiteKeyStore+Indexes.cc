//
//  SQLiteKeyStore+Indexes.cc
//  LiteCore
//
//  Created by Jens Alfke on 11/7/17.
//  Copyright © 2017 Couchbase. All rights reserved.
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

        if (type == KeyStore::kFullTextIndex) {
            // Full-text index can only have one key, so use that:
            if (params->count() != 1)
                error::_throw(error::InvalidQuery);
            params = params->get(0)->asArray();
            if (!params)
                error::_throw(error::InvalidQuery);
        }

        return {expressionFleece, params};
    }


    void SQLiteKeyStore::createIndex(slice indexName,
                                     slice expression,
                                     IndexType type,
                                     const IndexOptions *options) {
        validateIndexName(indexName);
        alloc_slice expressionFleece;
        const Array *params;
        tie(expressionFleece, params) = parseIndexExpr(expression, type);

        Transaction t(db());
        switch (type) {
            case  kValueIndex: {
                QueryParser qp(tableName());
                string indexNameStr = (string)indexName;
                qp.writeCreateIndex(indexNameStr, params);
                string sql = qp.SQL();
                SQLite::Statement getExistingSQL(db(), "SELECT sql FROM sqlite_master WHERE type='index' "
                                                "AND name=?");
                getExistingSQL.bind(1, indexNameStr);
                if(getExistingSQL.executeStep()) {
                    string existingSQL = getExistingSQL.getColumn(0).getString();
                    if(existingSQL == sql) {
                        return; // no-op
                    }
                }
                getExistingSQL.reset();

                _deleteIndex(indexName);
                db().exec(qp.SQL(), LogLevel::Info);
                break;
            }
            case kFullTextIndex:
                createFTSIndex(indexName, params, options);
                break;
            default:
                error::_throw(error::Unimplemented);
        }
        t.commit();
    }


    void SQLiteKeyStore::createFTSIndex(slice indexName,
                                        const Array *params,
                                        const IndexOptions *options)
    {
        // Check whether the index already exists:
        QueryParser qp(tableName());
        auto ftsTableName = qp.FTSTableName(string(indexName));

        // TODO: Check for no-op where FTS table already exists with identical properties/options

        // Delete any existing index:
        _deleteIndex(indexName);

        // Create the FTS4 table, with the tokenizer options: ( https://www.sqlite.org/fts3.html )
        stringstream sql;
        sql << "CREATE VIRTUAL TABLE \"" << ftsTableName << "\" USING fts4(text, tokenize=unicodesn";
        if (options) {
            if (options->stopWords) {
                string arg(options->stopWords);
                replace(arg, '"', ' ');
                replace(arg, ',', ' ');
                sql << " \"stopwordlist=" << arg << "\"";
            }
            if (options->language) {
                if (unicodesn_isSupportedStemmer(options->language)) {
                    sql << " \"stemmer=" << options->language << "\"";
                    if (!options->stopWords)
                        sql << " \"stopwords=" << options->language << "\"";
                } else {
                    Warn("FTS does not support language code '%s'; ignoring it",
                         options->language);
                }
            }
            if (options->ignoreDiacritics) {
                sql << " \"remove_diacritics=1\"";
            }
        }
        sql << ")";
        db().exec(sql.str(), LogLevel::Info);

        // Index existing records:
        db().exec("INSERT INTO \"" + ftsTableName + "\" (rowid, text) SELECT sequence, "
                  + QueryParser::expressionSQL(params, "body") + " FROM kv_" + name());

        // Set up triggers to keep the FTS5 table up to date:
        string ins = "INSERT INTO \"" + ftsTableName + "\" (rowid, text) VALUES (new.sequence, "
                    + QueryParser::expressionSQL(params, "new.body") + "); ";
        string del = "DELETE FROM \"" + ftsTableName + "\" WHERE rowid = old.sequence; ";

        db().exec(string("CREATE TRIGGER \"") + ftsTableName + "::ins\" AFTER INSERT ON kv_"
                  + name() + " BEGIN " + ins + " END");
        db().exec(string("CREATE TRIGGER \"") + ftsTableName + "::del\" AFTER DELETE ON kv_"
                  + name() + " BEGIN " + del + " END");
        db().exec(string("CREATE TRIGGER \"") + ftsTableName + "::upd\" AFTER UPDATE ON kv_"
                  + name() + " BEGIN " + del + ins + " END");
    }


    void SQLiteKeyStore::_deleteIndex(slice name) {
        // Delete any expression index:
        validateIndexName(name);
        string indexName = (string)name;
        db().exec(string("DROP INDEX IF EXISTS \"") + indexName + "\"", LogLevel::Info);

        // Delete any FTS index:
        QueryParser qp(tableName());
        auto ftsTableName = qp.FTSTableName(indexName);
        db().exec(string("DROP TABLE IF EXISTS \"") + ftsTableName + "\"", LogLevel::Info);
        db().exec(string("DROP TRIGGER IF EXISTS \"") + ftsTableName + "::ins\"");
        db().exec(string("DROP TRIGGER IF EXISTS \"") + ftsTableName + "::del\"");
        db().exec(string("DROP TRIGGER IF EXISTS \"") + ftsTableName + "::upd\"");
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
            db().execWithLock(string("CREATE UNIQUE INDEX IF NOT EXISTS kv_"+name()+"_seqs"
                                     " ON kv_"+name()+" (sequence)"));
            _createdSeqIndex = true;
        }
    }

}
