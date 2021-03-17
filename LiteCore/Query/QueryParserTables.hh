//
// QueryParserTables.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#pragma once
#include "QueryParser.hh"
#include "fleece/slice.hh"

namespace litecore {

    using namespace fleece; // enables ""_sl syntax
    using namespace fleece::impl;


    // This table defines the operators and their characteristics.
    // Each operator has a name, min/max argument count, precedence, and a handler method.
    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
    // http://www.sqlite.org/lang_expr.html
    typedef void (QueryParser::*OpHandler)(slice op, ArrayIterator& args);
    struct QueryParser::Operation {
        slice op; int minArgs; int maxArgs; int precedence; OpHandler handler;};
    const QueryParser::Operation QueryParser::kOperationList[] = {
        {"."_sl,       0, 9, 99,  &QueryParser::propertyOp},
        {"$"_sl,       1, 1, 99,  &QueryParser::parameterOp},
        {"?"_sl,       1, 9, 99,  &QueryParser::variableOp},
        {"_."_sl,      1, 2, 99,  &QueryParser::objectPropertyOp},
        {"[]"_sl,      0, 9, 99,  &QueryParser::arrayLiteralOp},
        {"BLOB"_sl,    1, 1, 99,  &QueryParser::blobOp},

        {"MISSING"_sl, 0, 0, 99,  &QueryParser::missingOp},

        {"||"_sl,      2, 9,  3,  &QueryParser::concatOp},       // converted to concat(...) call

        {"*"_sl,       2, 9,  7,  &QueryParser::infixOp},
        {"/"_sl,       2, 2,  7,  &QueryParser::infixOp},
        {"%"_sl,       2, 2,  7,  &QueryParser::infixOp},

        {"+"_sl,       2, 9,  6,  &QueryParser::infixOp},
        {"-"_sl,       2, 2,  6,  &QueryParser::infixOp},
        {"-"_sl,       1, 1,  9,  &QueryParser::prefixOp},

        {"<"_sl,       2, 2,  4,  &QueryParser::infixOp},
        {"<="_sl,      2, 2,  4,  &QueryParser::infixOp},
        {">"_sl,       2, 2,  4,  &QueryParser::infixOp},
        {">="_sl,      2, 2,  4,  &QueryParser::infixOp},

        {"="_sl,       2, 2,  3,  &QueryParser::infixOp},
        {"!="_sl,      2, 2,  3,  &QueryParser::infixOp},
        {"IS"_sl,      2, 2,  3,  &QueryParser::infixOp},
        {"IS NOT"_sl,  2, 2,  3,  &QueryParser::infixOp},
        {"IN"_sl,      2, 9,  3,  &QueryParser::inOp},
        {"LIKE"_sl,    2, 3,  3,  &QueryParser::likeOp},
        {"NOT IN"_sl,  2, 9,  3,  &QueryParser::inOp},
        {"MATCH"_sl,   2, 2,  3,  &QueryParser::matchOp},
        {"BETWEEN"_sl, 3, 3,  3,  &QueryParser::betweenOp},
        {"EXISTS"_sl,  1, 1,  8,  &QueryParser::existsOp},

        {"COLLATE"_sl, 2, 2, 10,  &QueryParser::collateOp},

        {"NOT"_sl,     1, 1,  9,  &QueryParser::prefixOp},
        {"AND"_sl,     2, 9,  2,  &QueryParser::infixOp},
        {"OR"_sl,      2, 9,  2,  &QueryParser::infixOp},

        {"CASE"_sl,    3, 9,  2,  &QueryParser::caseOp},

        {"ANY"_sl,     3, 3,  1,  &QueryParser::anyEveryOp},
        {"EVERY"_sl,   3, 3,  1,  &QueryParser::anyEveryOp},
        {"ANY AND EVERY"_sl, 3, 3,  1,  &QueryParser::anyEveryOp},

        {"SELECT"_sl,  1, 1,  1,  &QueryParser::selectOp},

        {"ASC"_sl,     1, 1,  2,  &QueryParser::postfixOp},
        {"DESC"_sl,    1, 1,  2,  &QueryParser::postfixOp},
        
        {"META"_sl,    0, 1,  99, &QueryParser::metaOp},

        {nullslice,    0, 0, 99,  &QueryParser::fallbackOp} // fallback; must come last in list
    };

    const QueryParser::Operation QueryParser::kArgListOperation
        {","_sl,       0, 9, -2, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kColumnListOperation
        {","_sl,       0, 9, -2, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kResultListOperation
        {","_sl,       0, 9, -2, &QueryParser::resultOp};
    const QueryParser::Operation QueryParser::kExpressionListOperation
        {nullslice,    1, 9, -3, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kOuterOperation
        {nullslice,    1, 1, -1};
    const QueryParser::Operation QueryParser::kHighPrecedenceOperation
        {nullslice,    1, 1, 10};


    // https://developer.couchbase.com/documentation/server/current/n1ql/n1ql-language-reference/functions.html
    // http://www.sqlite.org/lang_corefunc.html
    // http://www.sqlite.org/lang_aggfunc.html
    struct FunctionSpec {slice name; int minArgs; int maxArgs; slice sqlite_name; bool aggregate; bool wants_collation;};
    static const FunctionSpec kFunctionList[] = {
        // Array:
        {"array_agg"_sl,        1, 1},
        {"array_avg"_sl,        1, 1},
        {"array_contains"_sl,   2, 2},
        {"array_count"_sl,      1, 1},
        {"array_ifnull"_sl,     1, 1},
        {"array_length"_sl,     1, 1},
        {"array_max"_sl,        1, 1},
        {"array_min"_sl,        1, 1},
        {"array_of"_sl,         0, 9},
        {"array_sum"_sl,        1, 1},

        // Comparison:  (SQLite min and max are used in non-aggregate form here)
        {"greatest"_sl,         2, 9, "max"_sl},
        {"least"_sl,            2, 9, "min"_sl},

        // Conditional (unknowns):
        {"ifmissing"_sl,        2, 9, "coalesce"_sl},
        {"ifnull"_sl,           2, 9, "N1QL_ifnull"_sl},
        {"ifmissingornull"_sl,  2, 9},
        {"missingif"_sl,        2, 2},
        {"nullif"_sl,           2, 2, "N1QL_nullif"_sl},

        // Dates/times:
        { "millis_to_str"_sl,   1, 1 },
        { "millis_to_utc"_sl,   1, 1 },
        { "str_to_millis"_sl,   1, 1 },
        { "str_to_utc"_sl,      1, 1 },

        // Math:
        {"abs"_sl,              1, 1},
        {"acos"_sl,             1, 1},
        {"asin"_sl,             1, 1},
        {"atan"_sl,             1, 1},
        {"atan2"_sl,            2, 2},
        {"ceil"_sl,             1, 1},
        {"cos"_sl,              1, 1},
        {"degrees"_sl,          1, 1},
        {"e"_sl,                0, 0},
        {"exp"_sl,              1, 1},
        {"floor"_sl,            1, 1},
        {"ln"_sl,               1, 1},
        {"log"_sl,              1, 1},
        {"pi"_sl,               0, 0},
        {"power"_sl,            2, 2},
        {"radians"_sl,          1, 1},
        {"round"_sl,            1, 2},
        {"sign"_sl,             1, 1},
        {"sin"_sl,              1, 1},
        {"sqrt"_sl,             1, 1},
        {"tan"_sl,              1, 1},
        {"trunc"_sl,            1, 2},

        // Patterns:
        {"regexp_contains"_sl,  2, 2},
        {"regexp_like"_sl,      2, 2},
        {"regexp_position"_sl,  2, 2},
        {"regexp_replace"_sl,   3, 9},
        {"fl_like"_sl,          2, 2, nullslice, false, true},

        // Strings:
        {"concat"_sl,           2, 9},
        {"contains"_sl,         2, 2, nullslice, false, true},
        {"length"_sl,           1, 1, "N1QL_length"_sl},
        {"lower"_sl,            1, 1, "N1QL_lower"_sl},
        {"ltrim"_sl,            1, 2, "N1QL_ltrim"_sl},
        {"rtrim"_sl,            1, 2, "N1QL_rtrim"_sl},
        {"trim"_sl,             1, 2, "N1QL_trim"_sl},
        {"upper"_sl,            1, 1, "N1QL_upper"_sl},

        // Types:
        {"isarray"_sl,          1, 1},
        {"isatom"_sl,           1, 1},
        {"isboolean"_sl,        1, 1},
        {"isnumber"_sl,         1, 1},
        {"isobject"_sl,         1, 1},
        {"isstring"_sl,         1, 1},
        {"type"_sl,             1, 1},
        {"toarray"_sl,          1, 1},
        {"toatom"_sl,           1, 1},
        {"toboolean"_sl,        1, 1},
        {"tonumber"_sl,         1, 1},
        {"toobject"_sl,         1, 1},
        {"tostring"_sl,         1, 1},
        {"is_valued"_sl,        1, 1, "isvalued"_sl},

        // FTS (not standard N1QL):
        {"rank"_sl,             1, 1},

        // Aggregate functions:
        {"avg"_sl,              1, 1, nullslice, true},
        {"count"_sl,            0, 1, nullslice, true},
        {"max"_sl,              1, 1, nullslice, true},
        {"min"_sl,              1, 1, nullslice, true},
        {"sum"_sl,              1, 1, nullslice, true},

        // Predictive query:
#ifdef COUCHBASE_ENTERPRISE
        {"prediction"_sl,         2, 3},
        {"euclidean_distance"_sl, 2, 3},
        {"cosine_distance"_sl,    2, 2},
#endif

        {nullslice} // End of data
    };


    enum JoinType {
        kInvalidJoin = -1,
        kInner = 0,
        kLeft,
        kLeftOuter,
        kCross
    };

    static const char* const kJoinTypeNames[] = {
        "INNER",
        "LEFT",
        "LEFT OUTER",
        "CROSS",
        nullptr // End of data
    };

}
