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

#ifdef COUCHBASE_ENTERPRISE

#    include <cstdio>

#    include "SQLiteKeyStore.hh"
#    include "SQLiteDataFile.hh"
#    include "QueryParser.hh"
#    include "SQLUtil.hh"
#    include "StringUtil.hh"
#    include "Array.hh"
#    include "Error.hh"
#    include "SQLiteCpp/Exception.h"
#    include <sstream>

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

    // Vector search index for ML / predictive query, using the vectorsearch extension.
    // https://github.com/couchbaselabs/mobile-vector-search/blob/main/README_Extension.md

    static constexpr const char* kMetricNames[] = {nullptr, "euclidean2", "cosine"};

    /// Returns the SQL expression to create a vectorsearch virtual table.
    static string createVectorSearchTableSQL(string_view vectorTableName, const IndexSpec& spec) {
        stringstream stmt;
        stmt << "CREATE VIRTUAL TABLE " << sqlIdentifier(vectorTableName) << " USING vectorsearch(";
        Assert(spec.vectorOptions() != nullptr);
        IndexSpec::VectorOptions const& options = *spec.vectorOptions();
        stmt << "dimensions=" << options.dimensions << ',';
        if ( options.metric != IndexSpec::VectorOptions::DefaultMetric ) {
            stmt << "metric=" << kMetricNames[options.metric] << ',';
        }
        switch ( options.clustering.type ) {
            case IndexSpec::VectorOptions::Flat:
                stmt << "clustering=flat" << options.clustering.flat_centroids << ',';
                break;
            case IndexSpec::VectorOptions::Multi:
                stmt << "clustering=multi" << options.clustering.multi_subquantizers << 'x'
                     << options.clustering.multi_bits << ',';
                break;
            default:
                error::_throw(error::InvalidParameter, "invalid vector clustering type");
        }
        switch ( options.encoding.type ) {
            case IndexSpec::VectorOptions::DefaultEncoding:
                break;
            case IndexSpec::VectorOptions::NoEncoding:
                stmt << "encoding=none,";
                break;
            case IndexSpec::VectorOptions::PQ:
                stmt << "encoding=PQ" << options.encoding.pq_subquantizers << 'x' << options.encoding.bits << ',';
                break;
            case IndexSpec::VectorOptions::SQ:
                stmt << "encoding=SQ" << options.encoding.bits << ',';
                break;
            default:
                error::_throw(error::InvalidParameter, "invalid vector encoding type");
        }
        if ( options.numProbes > 0 ) stmt << "probes=" << options.numProbes << ',';
        if ( options.maxTrainingSize > 0 ) stmt << "maxToTrain=" << options.maxTrainingSize << ',';
        stmt << "minToTrain=" << options.minTrainingSize;
        stmt << ")";
        return stmt.str();
    }

    // Creates a vector-similarity index.
    bool SQLiteKeyStore::createVectorIndex(const IndexSpec& spec) {
        auto vectorTableName = db().auxiliaryTableName(tableName(), KeyStore::kVectorSeparator, spec.name);

        // Generate a SQL expression to get the vector:
        QueryParser qp(db(), collectionName(), tableName());
        qp.setBodyColumnName("new.body");
        string vectorExpr;
        if ( auto what = spec.what(); what && what->count() == 1 )
            vectorExpr = qp.vectorToIndexExpressionSQL(what->get(0), spec.vectorOptions()->dimensions);
        else
            error::_throw(error::Unimplemented, "Vector index doesn't support multiple properties");

        // Create the virtual table:
        try {
            if ( !db().createIndex(spec, this, vectorTableName, createVectorSearchTableSQL(vectorTableName, spec)) )
                return false;
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

        auto where = spec.where();
        qp.setBodyColumnName("body");
        string whereNewSQL = qp.whereClauseSQL(where, "new");
        string whereOldSQL = qp.whereClauseSQL(where, "old");

        // Index the existing records:
        db().exec(CONCAT("INSERT INTO " << sqlIdentifier(vectorTableName) << " (docid, vector)"
                                        << " SELECT new.rowid, " << vectorExpr << " AS vec FROM " << quotedTableName()
                                        << " AS new " << whereNewSQL << (whereNewSQL.empty() ? "WHERE" : " AND")
                                        << " vec NOT NULL"));

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

        // ...on delete:
        string deleteOldSQL = CONCAT("DELETE FROM " << sqlIdentifier(vectorTableName) << " WHERE docid = old.rowid");
        createTrigger(vectorTableName, "del", "AFTER DELETE", whereOldSQL, deleteOldSQL);

        // ...on update:
        createTrigger(vectorTableName, "preupdate", "BEFORE UPDATE OF body", whereOldSQL, deleteOldSQL);
        createTrigger(vectorTableName, "postupdate", "AFTER UPDATE OF body", whereNewSQL, insertNewSQL);
        return true;
    }

    string SQLiteKeyStore::findVectorIndexNameFor(const string& expressionJSON) {
        for ( IndexSpec const& index : getIndexes() ) {
            if ( index.type == IndexSpec::kVector ) {
                if ( index.what()->get(0)->toJSONString() == expressionJSON ) return index.name;
            }
        }
        return "";  // no index found
    }

}  // namespace litecore

#endif
