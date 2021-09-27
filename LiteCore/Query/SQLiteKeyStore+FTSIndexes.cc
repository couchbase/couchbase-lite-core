//
// SQLiteKeyStore+FTSIndexes.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include <stdio.h>

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "QueryParser.hh"
#include "StringUtil.hh"
#include "Array.hh"
#include <sstream>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"
}

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    static void writeTokenizerOptions(stringstream &sql, const IndexSpec::Options*);


    // Creates a FTS index.
    bool SQLiteKeyStore::createFTSIndex(const IndexSpec &spec)
    {
        auto ftsTableName = db().FTSTableName(tableName(), spec.name);
        // Collect the name of each FTS column and the SQL expression that populates it:
        QueryParser qp(db(), tableName());
        qp.setBodyColumnName("new.body");
        vector<string> colNames, colExprs;
        for (Array::iterator i(spec.what()); i; ++i) {
            colNames.push_back(CONCAT('"' << QueryParser::FTSColumnName(i.value()) << '"'));
            colExprs.push_back(qp.FTSExpressionSQL(i.value()));
        }
        string columns = join(colNames, ", ");
        string exprs = join(colExprs, ", ");

        auto where = spec.where();
        qp.setBodyColumnName("body");
        string whereNewSQL = qp.whereClauseSQL(where, "new");
        string whereOldSQL = qp.whereClauseSQL(where, "old");

        // Build the SQL that creates an FTS table, including the tokenizer options:
        {
            stringstream sql;
            sql << "CREATE VIRTUAL TABLE \"" << ftsTableName << "\" USING fts4(" << columns << ", ";
            writeTokenizerOptions(sql, spec.optionsPtr());
            sql << ")";
            if (!db().createIndex(spec, this, ftsTableName, sql.str()))
                return false;
        }

        // Index the existing records:
        db().exec(CONCAT("INSERT INTO \"" << ftsTableName << "\" (docid, " << columns << ") "
                         "SELECT rowid, " << exprs << " FROM kv_" << name() << " AS new "
                         << whereNewSQL));

        // Set up triggers to keep the FTS table up to date
        // ...on insertion:
        string insertNewSQL = CONCAT("INSERT INTO \"" << ftsTableName
                                     << "\" (docid, " << columns << ") "
                                     "VALUES (new.rowid, " << exprs << ")");
        createTrigger(ftsTableName, "ins",
                      "AFTER INSERT",
                      whereNewSQL,
                      insertNewSQL);

        // ...on delete:
        string deleteOldSQL = CONCAT("DELETE FROM \"" << ftsTableName << "\" WHERE docid = old.rowid");
        createTrigger(ftsTableName, "del",
                      "AFTER DELETE",
                      whereOldSQL,
                      deleteOldSQL);

        // ...on update:
        createTrigger(ftsTableName, "preupdate",
                      "BEFORE UPDATE OF body",
                      whereOldSQL,
                      deleteOldSQL);
        createTrigger(ftsTableName, "postupdate",
                      "AFTER UPDATE OF body",
                      whereNewSQL,
                      insertNewSQL);
        return true;
    }


    // subroutine that generates the option string passed to the FTS tokenizer
    static void writeTokenizerOptions(stringstream &sql, const IndexSpec::Options *options) {
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
            if (!options->ignoreDiacritics) {
                sql << " \"remove_diacritics=0\"";
            }
        }
    }

}
