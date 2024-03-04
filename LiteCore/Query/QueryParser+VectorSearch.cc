//
// QueryParser+VectorSearch.cc
//
// Copyright 2023-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#include "QueryParser.hh"
#include "QueryParser+Private.hh"
#include "SQLUtil.hh"
#include "StringUtil.hh"
#include "Dict.hh"
#include "Logging.hh"
#include "MutableArray.hh"

#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;
using namespace fleece::impl;
using namespace litecore::qp;

namespace litecore {

    static constexpr unsigned kDefaultMaxResults = 3;
    static constexpr unsigned kMaxMaxResults     = 10000;

    // Scans the entire query for vector_match() calls, and adds CTE tables for ones that are
    // indexed. These CTE tables will be JOINed, and accessed by writeVectorDistanceFn.
    void QueryParser::addVectorSearchCTEs(const Value* root) {
        unsigned n = 0;
        findNodes(root, kVectorMatchFnNameWithParens, 1, [&](const Array* matchExpr) {
            // Arguments to vector_match are index name, target vector, and optional max-results.
            string  tableName         = vectorIndexTableName(matchExpr->get(1), "vector_match");
            auto    targetVectorParam = matchExpr->get(2);
            int64_t maxResults        = kDefaultMaxResults;

            if ( matchExpr->count() > 3 ) {
                auto m = matchExpr->get(3);
                require(m->isInteger(), "max_results parameter to vector_match must be an integer");
                maxResults = m->asInt();
                require(maxResults > 0, "max_results parameter to vector_match must be positive");
                require(maxResults <= kMaxMaxResults, "max_results parameter to vector_match exceeds %u",
                        kMaxMaxResults);
            }

            const string& alias = indexJoinTableAlias(tableName, "vector");
            _sql << (n++ ? ", " : "WITH ");
            _sql << alias << " AS( SELECT rowid, distance FROM \"" << tableName
                 << "\" WHERE vector LIKE encode_vector(";
            parseNode(targetVectorParam);
            _sql << ") LIMIT " << maxResults << ") ";
        });
    }

    void QueryParser::writeVectorMatchFn(ArrayIterator& params) {
        // The work of `vector_match` is done by the JOIN, which limits the results to the
        // rowids produced by the `vss_search` CTE. So replace the call with a `true`.
        _sql << "true";
    }

    // Writes the SQL translation of the `vector_distance(...)` call,
    // which may or may not be indexed.
    void QueryParser::writeVectorDistanceFn(ArrayIterator& params) {
        string tableName = vectorIndexTableName(params[0], "vector_distance");
        // result is just the `distance` column of the CTE table.
        _sql << indexJoinTableAlias(tableName) << ".distance";
    }

    // Subroutine of addVectorSearchCTEs and writeVectorDistanceFn.
    // Given the first argument to `vector_match()` or `euclidean_distance` -- a property path
    // or other expression returning a vector -- returns the name of the sql-vss virtual table
    // indexing that expression, or "" if none.
    string QueryParser::vectorIndexTableName(const Value* match, const char* forFn) {
        string table = FTSTableName(match, true).first;
        require(_delegate.tableExists(table), "'%s' test requires a vector index", forFn);
        return table;
    }

    // Given a property path in a vector index expression,
    // returns the SQL of the value to be indexed in a document: the property value encoded as
    // a binary vector.
    std::string QueryParser::vectorToIndexExpressionSQL(const fleece::impl::Value* exprToIndex, unsigned dimensions) {
        auto a = MutableArray::newArray();
        a->append(dimensions);
        const Value* dimAsFleece = a->get(0);
        return functionCallSQL(kVectorToIndexFnName, exprToIndex, dimAsFleece);
    }

}  // namespace litecore

#endif  // COUCHBASE_ENTERPRISE
