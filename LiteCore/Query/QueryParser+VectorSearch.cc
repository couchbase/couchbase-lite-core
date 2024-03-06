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

#ifdef COUCHBASE_ENTERPRISE

using namespace std;
using namespace fleece;
using namespace fleece::impl;
using namespace litecore::qp;

namespace litecore {

    static constexpr unsigned kDefaultMaxResults = 10;

    // Scans the entire query for vector_match() calls, and adds CTE tables for ones that are
    // indexed. These CTE tables will be JOINed, and accessed by writeVectorDistanceFn.
    void QueryParser::addVectorSearchCTEs(const Value* root) {
        unsigned n = 0;
        findNodes(root, kVectorMatchFnNameWithParens, 1, [&](const Array* matchExpr) {
            // Arguments to vector_match are property path, target vector, and optional max-results.
            auto    propertyParam     = matchExpr->get(1);
            auto    targetVectorParam = matchExpr->get(2);
            int64_t maxResults        = kDefaultMaxResults;

            string tableName = vectorIndexTableName(propertyParam);
            require(!tableName.empty(), "There is no vector-search index on property %s",
                    matchExpr->get(1)->toJSONString().c_str());

            if ( matchExpr->count() > 3 ) {
                auto m = matchExpr->get(3);
                require(m->isInteger(), "max_results parameter to vector_match must be an integer");
                maxResults = m->asInt();
                require(maxResults > 0, "max_results parameter to vector_match must be positive");
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
        requireTopLevelConjunction("VECTOR_MATCH");
        // The work of `vector_match` is done by the JOIN, which limits the results to the
        // rowids produced by the `vss_search` CTE. So replace the call with a `true`.
        _sql << "true";
    }

    // Writes the SQL translation of the `vector_distance(...)` call,
    // which may or may not be indexed.
    void QueryParser::writeVectorDistanceFn(ArrayIterator& params) {
        string tableName = vectorIndexTableName(params[0]);
        if ( !tableName.empty() ) {
            // Indexed; result is just the `distance` column of the CTE table.
            _sql << indexJoinTableAlias(tableName) << ".distance";
        } else {
            // No index, so call the regular distance fn.
            // vectorsearch extn returns *squared* Euclidean distance, so for compatibility add a '2'
            // parameter to the euclidean_distance() call.
            LogWarn(QueryLog,
                    "No vector index for property %s; "
                    "vector_distance() call will fall back to linear scan",
                    params[0]->toJSONString().c_str());
            _sql << "euclidean_distance(";
            _context.push_back(&kExpressionListOperation);  // suppresses parens around arg list
            writeArgList(params);
            _context.pop_back();
            _sql << ", 2)";
        }
    }

    // Subroutine of addVectorSearchCTEs and writeVectorDistanceFn.
    // Given the first argument to `vector_match()` or `euclidean_distance` -- a property path
    // or other expression returning a vector -- returns the name of the sql-vss virtual table
    // indexing that expression, or "" if none.
    string QueryParser::vectorIndexTableName(const Value* match) {
        if ( string ftsTable = FTSTableName(match, true).first; !ftsTable.empty() ) return ftsTable;
        return _delegate.vectorTableName(_defaultTableName, match->toJSONString());
    }

    // Given a property path in a vector index expression,
    // returns the SQL of the value to be indexed in a document: the property value encoded as
    // a binary vector.
    std::string QueryParser::vectorExpressionSQL(const fleece::impl::Value* exprToIndex) {
        return functionCallSQL(kVectorValueFnName, exprToIndex);
    }

}  // namespace litecore

#endif  // COUCHBASE_ENTERPRISE
