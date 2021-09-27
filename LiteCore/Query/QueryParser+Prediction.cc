#ifdef COUCHBASE_ENTERPRISE

//
// QuerParser+Prediction.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#include "QueryParser.hh"
#include "QueryParser+Private.hh"
#include "FleeceImpl.hh"
#include "StringUtil.hh"
#include "Path.hh"

using namespace std;
using namespace fleece;
using namespace fleece::impl;
using namespace litecore::qp;

namespace litecore {


    // Scans the entire query for PREDICTION() calls and adds join tables for ones that are indexed.
    void QueryParser::findPredictionCalls(const Value *root) {
        findNodes(root, kPredictionFnNameWithParens, 1, [this](const Array *pred) {
            predictiveJoinTableAlias(pred, true);
        });
    }


    // Looks up or adds a join alias for a predictive index table.
    const string& QueryParser::predictiveJoinTableAlias(const Value *predictionExpr, bool canAdd) {
        string table = predictiveTableName(predictionExpr);
        if (canAdd && !_delegate.tableExists(table))
            canAdd = false; // not indexed
        return indexJoinTableAlias(table, (canAdd ? "pred" : nullptr));
    }


    // Constructs a unique identifier of a specific PREDICTION() call, from a digest of its JSON.
    string QueryParser::predictiveIdentifier(const Value *expression) const {
        auto array = expression->asArray();
        if (!array || array->count() < 2
                   || !array->get(0)->asString().caseEquivalent(kPredictionFnNameWithParens))
            fail("Invalid PREDICTION() call");
        return expressionIdentifier(array, 3);     // ignore the output-property parameter
    }


    // Returns the name of the index table for a PREDICTION() call expression.
    string QueryParser::predictiveTableName(const Value *expression) const {
        string table = _defaultTableName; //TEMP
        return _delegate.predictiveTableName(table, predictiveIdentifier(expression));
    }


    bool QueryParser::writeIndexedPrediction(const Array *node) {
        auto alias = predictiveJoinTableAlias(node);
        if (alias.empty())
            return false;
        if (node->count() >= 4) {
            slice property = requiredString(node->get(3), "PREDICTION() property name");
            _sql << kUnnestedValueFnName << "(" << alias << ".body, "
                 << sqlString(string(Path(property)));
            _sql << ")";
        } else {
            _sql << kRootFnName << "(" << alias << ".body)";
        }
        return true;
    }

}

#endif // COUCHBASE_ENTERPRISE
