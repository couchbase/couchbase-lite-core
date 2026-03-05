//
// SQLiteKeyStore+VectorIndex.cc
//
// Copyright 2023-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include <cstdio>

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "QueryTranslator.hh"
#include "SQLUtil.hh"
#include "SQLite_Internal.hh"
#include "StringUtil.hh"
#include "Array.hh"
#include "Error.hh"
#include "SQLiteCpp/Statement.h"
#include "SQLiteCpp/Exception.h"
#include <sstream>

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

#ifdef COUCHBASE_ENTERPRISE

    // Vector search index for ML / predictive query, using the vectorsearch extension.
    // https://github.com/couchbaselabs/mobile-vector-search/blob/main/docs/Extension.md

    // Creates a vector-similarity index.
    bool SQLiteKeyStore::createVectorIndex(const IndexSpec& spec) {
        auto vectorTableName = db().auxiliaryTableName(tableName(), KeyStore::kVectorSeparator, spec.name);
        auto vectorOptions   = spec.vectorOptions();
        Assert(vectorOptions);

        // Generate a SQL expression to get the vector:
        QueryTranslator qp(db(), collectionName(), tableName());
        qp.setBodyColumnName("new.body");
        string vectorExpr;
        if ( auto what = (const Array*)spec.what(); what && what->count() == 1 ) {
            vectorExpr = qp.vectorToIndexExpressionSQL(FLValue(what->get(0)), vectorOptions->dimensions);
        } else {
            error::_throw(error::Unimplemented, "Vector index doesn't support multiple properties");
        }

        // Create the virtual table:
        try {
            string sql = CONCAT("CREATE VIRTUAL TABLE " << sqlIdentifier(vectorTableName) << " USING vectorsearch("
                                                        << *vectorOptions << ")");
            if ( !db().createIndex(spec, this, vectorTableName, sql) ) return false;
        } catch ( SQLite::Exception const& x ) {
            string_view what(x.what());
            if ( hasPrefix(what, "no such module") ) {
                error::_throw(error::Unimplemented, "CouchbaseLiteVectorSearch extension is not installed");
            } else if ( hasPrefix(x.what(), "vectorsearch: ") ) {
                what = what.substr(14);
                // SQLiteDataFile.exec appends "--" and the SQL; remove that (kludgily)
                if ( auto dash = what.find(" -- "); dash != string::npos ) what = what.substr(0, dash);
                error::_throw(error::InvalidParameter, "%.*s", FMTSLICE(slice(what)));
            } else {
                throw;
            }
        }

        // Create an AFTER DELETE trigger to remove any vector from the index:
        auto where = spec.where();
        qp.setBodyColumnName("body");
        string whereNewSQL  = qp.whereClauseSQL(FLValue(where), "new");
        string whereOldSQL  = qp.whereClauseSQL(FLValue(where), "old");
        string deleteOldSQL = CONCAT("DELETE FROM " << sqlIdentifier(vectorTableName) << " WHERE docid = old.rowid");

        // Always delete obsolete vectors when a doc is updated or deleted:
        createTrigger(vectorTableName, "preupdate", "BEFORE UPDATE OF body", whereOldSQL, deleteOldSQL);
        createTrigger(vectorTableName, "del", "AFTER DELETE", whereOldSQL, deleteOldSQL);

        bool lazy = vectorOptions->lazyEmbedding;
        if ( lazy ) {
            // Lazy index: Mark as lazy by initializing lastSeq. Vectors will not be computed
            // automatically; app updates them via the LazyIndex class.
            db().setIndexSequences(spec.name, "[]");
        } else {
            // Index the existing records:
            db().exec(CONCAT("INSERT INTO " << sqlIdentifier(vectorTableName) << " (docid, vector)"
                                            << " SELECT new.rowid, " << vectorExpr << " AS vec FROM "
                                            << quotedTableName() << " AS new " << whereNewSQL
                                            << (whereNewSQL.empty() ? "WHERE" : " AND") << " vec NOT NULL"));

            // Update the `where` condition to skip docs that don't have a vector:
            if ( whereNewSQL.empty() ) whereNewSQL = "WHERE";
            else
                whereNewSQL += " AND";
            whereNewSQL += " (" + vectorExpr + ") NOT NULL";

            // Set up triggers to keep the virtual table up to date
            // ...on insertion:
            string insertNewSQL = CONCAT("INSERT INTO " << sqlIdentifier(vectorTableName)
                                                        << " (docid, vector) "
                                                           "VALUES (new.rowid, "
                                                        << vectorExpr << ")");
            createTrigger(vectorTableName, "ins", "AFTER INSERT", whereNewSQL, insertNewSQL);

            // ...on update:
            createTrigger(vectorTableName, "postupdate", "AFTER UPDATE OF body", whereNewSQL, insertNewSQL);
        }

        return true;
    }

    string SQLiteKeyStore::findVectorIndexNameFor(const string& expressionJSON) {
        for ( IndexSpec const& index : getIndexes() ) {
            if ( index.type == IndexSpec::kVector ) {
                auto what = (const Array*)index.what();
                if ( what->get(0)->toJSONString() == expressionJSON ) return index.name;
            }
        }
        return "";  // no index found
    }

    // The opposite of createVectorSearchTableSQL
    optional<IndexSpec::VectorOptions> SQLiteKeyStore::parseVectorSearchTableSQL(string_view sql) {
        // Find the virtual-table arguments in the CREATE TABLE statement:
        auto start = sql.find("vectorsearch(");
        if ( start == string::npos ) return nullopt;
        start += strlen("vectorsearch(");
        auto end = sql.find(')', start);
        if ( end == string::npos ) return nullopt;

        // Parse each comma-delimited key-value pair:
        string_view              args(&sql[start], end - start);
        IndexSpec::VectorOptions opts;
        split(args, ",", [&](string_view arg) { (void)opts.readArg(arg); });
        return opts;
    }

#endif  // COUCHBASE_ENTERPRISE

    bool SQLiteKeyStore::isIndexTrained(fleece::slice name) const {
        if ( auto spec = db().getIndex(name); spec && spec->keyStoreName == this->name() ) {
            if ( spec->type != IndexSpec::kVector ) {
                error::_throw(error::InvalidParameter, "Index '%.*s' is not a vector index", SPLAT(name));
            }
            auto q = db().compile(
                    ("SELECT 1 FROM \""s + spec->indexTableName + "\" WHERE bucket != -1 LIMIT 1").c_str());
            return q->executeStep();
        }

        error::_throw(error::NoSuchIndex);
    }

}  // namespace litecore
