//
//  QueryParserTables.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "QueryParser.hh"
#include "slice.hh"

namespace litecore {

    using namespace fleece; // enables ""_sl syntax


    // This table defines the operators and their characteristics.
    // Each operator has a name, min/max argument count, precedence, and a handler method.
    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
    // http://www.sqlite.org/lang_expr.html
    typedef void (QueryParser::*OpHandler)(slice op, Array::iterator& args);
    struct QueryParser::Operation {
        slice op; int minArgs; int maxArgs; int precedence; OpHandler handler;};
    const QueryParser::Operation QueryParser::kOperationList[] = {
        {"."_sl,       1, 9,  9,  &QueryParser::propertyOp},
        {"$"_sl,       1, 1,  9,  &QueryParser::parameterOp},
        {"?"_sl,       1, 9,  9,  &QueryParser::variableOp},

        {"MISSING"_sl, 0, 0,  9,  &QueryParser::missingOp},

        {"||"_sl,      2, 9,  8,  &QueryParser::infixOp},

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
        {"NOT IN"_sl,  2, 9,  3,  &QueryParser::inOp},
        {"LIKE"_sl,    2, 2,  3,  &QueryParser::infixOp},
        {"MATCH"_sl,   2, 2,  3,  &QueryParser::matchOp},
        {"BETWEEN"_sl, 3, 3,  3,  &QueryParser::betweenOp},
        {"EXISTS"_sl,  1, 1,  8,  &QueryParser::existsOp},

        {"NOT"_sl,     1, 1,  9,  &QueryParser::prefixOp},
        {"AND"_sl,     2, 9,  2,  &QueryParser::infixOp},
        {"OR"_sl,      2, 9,  2,  &QueryParser::infixOp},

        {"ANY"_sl,     3, 3,  1,  &QueryParser::anyEveryOp},
        {"EVERY"_sl,   3, 3,  1,  &QueryParser::anyEveryOp},
        {"ANY AND EVERY"_sl, 3, 3,  1,  &QueryParser::anyEveryOp},

        {"SELECT"_sl,  1, 1,  1,  &QueryParser::selectOp},

        {"DESC"_sl,    1, 1,  2,  &QueryParser::postfixOp},

        {nullslice,    0, 0, 10,  &QueryParser::fallbackOp} // fallback; must come last
    };

    const QueryParser::Operation QueryParser::kArgListOperation
        {","_sl,       0, 9, -2, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kColumnListOperation
        {","_sl,       0, 9, -2, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kOrderByOperation
        {"ORDER BY"_sl,1, 9, -3, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kOuterOperation
        {nullslice,    1, 1, -1};


    // https://developer.couchbase.com/documentation/server/current/n1ql/n1ql-language-reference/functions.html
    // http://www.sqlite.org/lang_corefunc.html
    // http://www.sqlite.org/lang_aggfunc.html
    struct FunctionSpec {slice name; int minArgs; int maxArgs; slice sqlite_name; bool aggregate;};
    static const FunctionSpec kFunctionList[] = {
        // Array:
        {"array_append"_sl,     2, 2},
        {"array_avg"_sl,        1, 1},
        {"array_concat"_sl,     2, 2},
        {"array_contains"_sl,   2, 2},
        {"array_count"_sl,      1, 1},
        {"array_distinct"_sl,   1, 1},
        {"array_ifnull"_sl,     1, 1},
        {"array_insert"_sl,     3, 3},
        {"array_intersect"_sl,  2, 9},
        {"array_length"_sl,     1, 1},
        {"array_max"_sl,        1, 1},
        {"array_min"_sl,        1, 1},
        {"array_position"_sl,   2, 2},
        {"array_prepend"_sl,    2, 2},
        {"array_put"_sl,        2, 2},
        {"array_range"_sl,      2, 3},
        {"array_remove"_sl,     2, 2},
        {"array_repeat"_sl,     2, 2},
        {"array_replace"_sl,    3, 4},
        {"array_reverse"_sl,    1, 1},
        {"array_sort"_sl,       1, 1},
        {"array_sum"_sl,        1, 1},
        {"array_star"_sl,       1, 1},

        // Comparison:
        {"greatest"_sl,         1, 9, "max"_sl},
        {"least"_sl,            1, 9, "min"_sl},

        // Conditional (unknowns):
        {"ifmissing"_sl,        2, 2},
        {"ifnull"_sl,           2, 2},
        {"ifmissingornull"_sl,  2, 2},
        {"missingif"_sl,        2, 2},
        {"nullif"_sl,           2, 2},

        // Conditional (numbers):
        {"ifinf"_sl,            1, 9},
        {"ifnan"_sl,            1, 9},
        {"ifnanorinf"_sl,       1, 9},
        {"nanif"_sl,            2, 2},
        {"neginfif"_sl,         2, 2},
        {"posinfif"_sl,         2, 2},

        // Meta and UUID:
        {"base64"_sl,           1, 1},
        {"base64_encode"_sl,    1, 1},
        {"base64_decode"_sl,    1, 1},
        {"meta"_sl,             1, 1},
        {"uuid"_sl,             0, 0},

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
        {"ln"_sl,               1, 1},
        {"log"_sl,              1, 1},
        {"floor"_sl,            1, 1},
        {"pi"_sl,               0, 0},
        {"power"_sl,            2, 2},
        {"radians"_sl,          1, 1},
        {"random"_sl,           0, 1},
        {"round"_sl,            1, 2},
        {"sign"_sl,             1, 1},
        {"sin"_sl,              1, 1},
        {"sqrt"_sl,             1, 1},
        {"tan"_sl,              1, 1},
        {"trunc"_sl,            1, 2},

        // Objects:
        {"object_length"_sl,    1, 1},
        {"object_names"_sl,     1, 1},
        {"object_pairs"_sl,     1, 1},
        {"object_length"_sl,    1, 1},
        {"object_inner_pairs"_sl,   1, 1},
        {"object_values"_sl,        1, 1},
        {"object_inner_values"_sl,  1, 1},
        {"object_add"_sl,       3, 3},
        {"object_put"_sl,       3, 3},
        {"object_remove"_sl,    2, 2},
        {"object_unwrap"_sl,    1, 1},

        // Patterns:
        {"regexp_contains"_sl,  2, 2},
        {"regexp_like"_sl,      2, 2},
        {"regexp_position"_sl,  2, 2},
        {"regexp_replace"_sl,   3, 9},

        // Strings:
        {"contains"_sl,         2, 2},
        {"initcap"_sl,          1, 1},
        {"length"_sl,           1, 1},
        {"lower"_sl,            1, 1},
        {"ltrim"_sl,            1, 2},
        {"position"_sl,         2, 2},
        {"repeat"_sl,           2, 2},
        {"replace"_sl,          3, 4},
        {"rtrim"_sl,            1, 2},
        {"split"_sl,            1, 2},
        {"substr"_sl,           2, 3},
        {"suffixes"_sl,         1, 1},
        {"title"_sl,            1, 1, "initcap"_sl}, // synonym for initcap
        {"trim"_sl,             1, 2},
        {"upper"_sl,            1, 1},

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
        {"upper"_sl,            1, 1},

        // FTS (not standard N1QL):
        {"rank"_sl,             1, 1},

        // Aggregate functions:
        {"array_agg"_sl,        1, 1, nullslice, true},
        {"avg"_sl,              1, 1, nullslice, true},
        {"count"_sl,            0, 1, nullslice, true},
        {"max"_sl,              1, 1, nullslice, true},
        {"min"_sl,              1, 1, nullslice, true},
        {"sum"_sl,              1, 1, nullslice, true},

        //TODO: Add date, JSON functions
        {nullslice}
    };

}
