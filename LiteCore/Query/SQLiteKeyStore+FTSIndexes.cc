//
// SQLiteKeyStore+FTSIndexes.cc
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

#include <stdio.h>

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "QueryParser.hh"
#include "StringUtil.hh"
#include <sstream>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"
}

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    static void writeTokenizerOptions(stringstream &sql, const KeyStore::IndexOptions*);


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

}
