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
#include "QueryParserTables.hh"
#include "Error.hh"
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

    // Scans the entire query for vector_match() calls, and adds join tables for ones that are
    // indexed.
    void QueryParser::addVectorSearchJoins(const Dict* select) {
        findNodes(select, kVectorMatchFnNameWithParens, 1, [&](const Array* matchExpr) {
            // Arguments to vector_match are index name and target vector.
            string         tableName         = FTSTableName(matchExpr->get(1), true).first;
            auto           targetVectorParam = matchExpr->get(2);
            indexJoinInfo* info              = indexJoinTable(tableName, "vector");
            if ( matchExpr == getCaseInsensitive(select, "WHERE") ) {
                // If vector_match is the entire WHERE clause, this is a simple non-hybrid query.
                // This is implemented by a nested SELECT that finds the nearest vectors in
                // the entire collection. Isolating this in a nested SELECT ensures SQLite doesn't
                // see the outer JOIN against the collection; if it did, the vectorsearch extension's
                // planner would see a constraint against `rowid` and interpret it as a hybrid search.
                // https://github.com/couchbaselabs/mobile-vector-search/blob/main/docs/Extension.md

                // Figure out the limit to use in the vector query:
                int64_t maxResults;
                if ( auto limitVal = getCaseInsensitive(select, "LIMIT") ) {
                    maxResults = limitVal->asInt();
                    require(limitVal->isInteger() && maxResults > 0,
                            "LIMIT must be a positive integer when using vector_match()");
                    require(maxResults <= kMaxMaxResults, "LIMIT must not exceed %u when using vector_match()",
                            kMaxMaxResults);
                } else {
                    maxResults = kDefaultMaxResults;
                }

                // Register a callback to write the nested SELECT in place of a table name:
                info->writeTableSQL = [=] {
                    _sql << "(SELECT rowid, distance FROM \"" << tableName << "\" WHERE vector MATCH encode_vector(";
                    parseNode(targetVectorParam);
                    _sql << ") LIMIT " << maxResults << ")";
                };
            }
        });
    }

    // Writes a `vector_match()` expression.
    void QueryParser::writeVectorMatchFn(ArrayIterator& params) {
        requireTopLevelConjunction("VECTOR_MATCH");
        auto parentCtx = _context.rbegin() + 1;
        auto parentOp  = (*parentCtx)->op;
        if ( parentOp == "SELECT"_sl || parentOp == nullslice ) {
            // In a simple query the work of `vector_match` is done by the JOIN, which limits the results to the
            // rowids produced by the nested query of the vector table.
            // Since there's nothing to do here, replace the call with a `true`.
            _sql << "true";
        } else {
            // In a hybrid query we do write the LIKE test at the point of the match call:
            string        tableName = FTSTableName(params[0], true).first;
            const string& alias     = indexJoinTableAlias(tableName, "vector");
            Assert(!alias.empty());
            auto targetVectorParam = params[1];
            _sql << sqlIdentifier(alias) << ".vector MATCH encode_vector(";
            parseNode(targetVectorParam);
            _sql << ")";
        }
    }

    // Writes the SQL translation of the `vector_distance(...)` call.
    void QueryParser::writeVectorDistanceFn(ArrayIterator& params) {
        string tableName = FTSTableName(params[0], true).first;
        _sql << indexJoinTableAlias(tableName, "vector") << ".distance";
    }

    // Given the expression to index from a vector index spec, returns the SQL of a
    // `fl_vector_to_index()` call whose value is a binary vector to pass to vectorsearch.
    std::string QueryParser::vectorToIndexExpressionSQL(const fleece::impl::Value* exprToIndex, unsigned dimensions) {
        auto a = MutableArray::newArray();
        a->append(dimensions);
        const Value* dimAsFleece = a->get(0);
        return functionCallSQL(kVectorToIndexFnName, exprToIndex, dimAsFleece);
    }

}  // namespace litecore

#endif  // COUCHBASE_ENTERPRISE
