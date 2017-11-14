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


    // Generates the SQL that creates an FTS table, including the tokenizer options.
    static string sqlToCreateFTSTable(const string &ftsTableName,
                                      const Array *params,
                                      const KeyStore::IndexOptions *options)
    {
        // ( https://www.sqlite.org/fts3.html )
        stringstream sql;
        sql << "CREATE VIRTUAL TABLE \"" << ftsTableName << "\" USING fts4(";
        for (Array::iterator i(params); i; ++i) {
            sql << '"' << QueryParser::FTSColumnName(i.value()) << "\", ";
        }

        // Add tokenizer options:
        sql << "tokenize=unicodesn";
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
        return sql.str();
    }


    // Creates a FTS index.
    void SQLiteKeyStore::createFTSIndex(string indexName,
                                        const Array *params,
                                        const IndexOptions *options)
    {
        // Create the FTS table, but if an identical one already exists, return:
        auto ftsTableName = QueryParser(tableName()).FTSTableName(indexName);
        if (!_createIndex(kFullTextIndex, ftsTableName, indexName,
                          sqlToCreateFTSTable(ftsTableName, params, options)))
            return;

        // Construct a string with the FTS table column names:
        stringstream sColumns;
        sColumns << "rowid";
        for (Array::iterator i(params); i; ++i)
            sColumns << ", \"" << QueryParser::FTSColumnName(i.value()) << "\"";
        string columns = sColumns.str();

        // Index the existing records:
        stringstream inSQL;
        inSQL << "INSERT INTO \"" << ftsTableName << "\" (" << columns << ") SELECT sequence";
        for (Array::iterator i(params); i; ++i)
            inSQL << ", " << QueryParser::expressionSQL(i.value(), "body");
        inSQL << " FROM kv_" << name();
        db().exec(inSQL.str());

        // Set up triggers to keep the FTS table up to date:
        stringstream ins, del;
        ins << "INSERT INTO \"" << ftsTableName << "\" (" << columns << ") VALUES (new.sequence";
        for (Array::iterator i(params); i; ++i)
            ins << ", " << QueryParser::expressionSQL(i.value(), "new.body");
        ins << "); ";

        del << "DELETE FROM \"" << ftsTableName << "\" WHERE rowid = old.sequence; ";

        db().exec(string("CREATE TRIGGER \"") + ftsTableName + "::ins\" AFTER INSERT ON kv_"
                  + name() + " BEGIN " + ins.str() + " END");
        db().exec(string("CREATE TRIGGER \"") + ftsTableName + "::del\" AFTER DELETE ON kv_"
                  + name() + " BEGIN " + del.str() + " END");
        db().exec(string("CREATE TRIGGER \"") + ftsTableName + "::upd\" AFTER UPDATE ON kv_"
                  + name() + " BEGIN " + del.str() + ins.str() + " END");
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
