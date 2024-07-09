//
// QueryParserTables.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "QueryParser.hh"
#include "fleece/slice.hh"

namespace litecore {

    using namespace fleece;
    using namespace fleece::impl;


    // This table defines the operations and their characteristics.
    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
    // http://www.sqlite.org/lang_expr.html

    typedef void (QueryParser::*OpHandler)(slice op, ArrayIterator& args);

    struct QueryParser::Operation {  // NOLINT(cppcoreguidelines-pro-type-member-init)
        slice     op;                // Name, as found in 1st item of array
        int       minArgs, maxArgs;  // Min/max number of args; max 9 means "unlimited"
        int       precedence;        // Used to minimize generated parens
        OpHandler handler;           // Method that parses this operation
    };

    inline constexpr QueryParser::Operation QueryParser::kOperationList[] = {
            {".", 0, 9, 99, &QueryParser::propertyOp},
            {"$", 1, 1, 99, &QueryParser::parameterOp},
            {"?", 1, 9, 99, &QueryParser::variableOp},
            {"_.", 1, 2, 99, &QueryParser::objectPropertyOp},
            {"[]", 0, 9, 99, &QueryParser::arrayLiteralOp},
            {"BLOB", 1, 1, 99, &QueryParser::blobOp},

            {"MISSING", 0, 0, 99, &QueryParser::missingOp},

            {"||", 2, 9, 3, &QueryParser::concatOp},  // converted to concat(...) call

            {"*", 2, 9, 7, &QueryParser::infixOp},
            {"/", 2, 2, 7, &QueryParser::infixOp},
            {"%", 2, 2, 7, &QueryParser::infixOp},

            {"+", 2, 9, 6, &QueryParser::infixOp},
            {"-", 2, 2, 6, &QueryParser::infixOp},
            {"-", 1, 1, 9, &QueryParser::prefixOp},

            {"<", 2, 2, 4, &QueryParser::infixOp},
            {"<=", 2, 2, 4, &QueryParser::infixOp},
            {">", 2, 2, 4, &QueryParser::infixOp},
            {">=", 2, 2, 4, &QueryParser::infixOp},

            {"=", 2, 2, 3, &QueryParser::infixOp},
            {"!=", 2, 2, 3, &QueryParser::infixOp},
            {"IS", 2, 2, 3, &QueryParser::infixOp},
            {"IS NOT", 2, 2, 3, &QueryParser::infixOp},
            {"IN", 2, 9, 3, &QueryParser::inOp},
            {"LIKE", 2, 3, 3, &QueryParser::likeOp},
            {"NOT IN", 2, 9, 3, &QueryParser::inOp},
            {"BETWEEN", 3, 3, 3, &QueryParser::betweenOp},
            {"EXISTS", 1, 1, 8, &QueryParser::existsOp},
            {"IS VALUED", 1, 1, 3, &QueryParser::functionOp},

            {"COLLATE", 2, 2, 10, &QueryParser::collateOp},

            {"NOT", 1, 1, 9, &QueryParser::prefixOp},
            {"AND", 2, 9, 2, &QueryParser::infixOp},
            {"OR", 2, 9, 2, &QueryParser::infixOp},

            {"CASE", 3, 9, 2, &QueryParser::caseOp},

            {"ANY", 3, 3, 1, &QueryParser::anyEveryOp},
            {"EVERY", 3, 3, 1, &QueryParser::anyEveryOp},
            {"ANY AND EVERY", 3, 3, 1, &QueryParser::anyEveryOp},

            {"SELECT", 1, 1, 1, &QueryParser::selectOp},

            {"ASC", 1, 1, 2, &QueryParser::postfixOp},
            {"DESC", 1, 1, 2, &QueryParser::postfixOp},

            {"META()", 0, 1, 99, &QueryParser::metaOp},

            {nullslice, 0, 0, 99, &QueryParser::fallbackOp}  // fallback; must come last in list
    };

    // Declarations of some operations that don't exist in the input but are synthesized internally:

    inline constexpr QueryParser::Operation QueryParser::kArgListOperation{",", 0, 9, -2, &QueryParser::infixOp};
    inline constexpr QueryParser::Operation QueryParser::kColumnListOperation{",", 0, 9, -2, &QueryParser::infixOp};
    inline constexpr QueryParser::Operation QueryParser::kResultListOperation{",", 0, 9, -2, &QueryParser::resultOp};
    inline constexpr QueryParser::Operation QueryParser::kExpressionListOperation{nullslice, 1, 9, -3,
                                                                                  &QueryParser::infixOp};
    inline constexpr QueryParser::Operation QueryParser::kOuterOperation{"outer", 1, 1, -1};
    inline constexpr QueryParser::Operation QueryParser::kHighPrecedenceOperation{"high prec", 1, 1, 10};
    inline constexpr QueryParser::Operation QueryParser::kWhereOperation{"WHERE", 1, 1, -1};

    // Table of functions. Used when the 1st item of the array ends with "()".
    // https://developer.couchbase.com/documentation/server/current/n1ql/n1ql-language-reference/functions.html
    // http://www.sqlite.org/lang_corefunc.html
    // http://www.sqlite.org/lang_aggfunc.html

    struct FunctionSpec {        // NOLINT(cppcoreguidelines-pro-type-member-init)
        slice name;              // Name (without the parens)
        int   minArgs, maxArgs;  // Min/max number of args; max 9 means "unlimited"
        slice sqlite_name;       // Name to use in SQL; defaults to `name`
        bool  aggregate;         // Is this an aggregate function?
        bool  wants_collation;   // Does this function support a collation argument?
    };

    inline static constexpr FunctionSpec kFunctionList[] = {
            // Array:
            {"array_agg", 1, 1},
            {"array_avg", 1, 1},
            {"array_contains", 2, 2},
            {"array_count", 1, 1},
            {"array_ifnull", 1, 1},
            {"array_length", 1, 1},
            {"array_max", 1, 1},
            {"array_min", 1, 1},
            {"array_of", 0, 9},
            {"array_sum", 1, 1},

            // Comparison:  (SQLite min and max are used in non-aggregate form here)
            {"greatest", 2, 9, "max"},
            {"least", 2, 9, "min"},

            // Conditional (unknowns):
            {"ifmissing", 2, 9, "coalesce"},
            {"ifnull", 2, 9, "N1QL_ifnull"},
            {"ifmissingornull", 2, 9},
            {"missingif", 2, 2},
            {"nullif", 2, 2, "N1QL_nullif"},

            // Dates/times:
            {"millis_to_str", 1, 2},
            {"millis_to_utc", 1, 2},
            {"millis_to_tz", 2, 3},
            {"str_to_millis", 1, 1},
            {"str_to_utc", 1, 2},
            {"date_diff_str", 3, 3},
            {"date_diff_millis", 3, 3},
            {"date_add_str", 3, 4},
            {"date_add_millis", 3, 3},
            {"str_to_tz", 2, 3},

            // Math:
            {"abs", 1, 1},
            {"acos", 1, 1},
            {"asin", 1, 1},
            {"atan", 1, 1},
            {"atan2", 2, 2},
            {"ceil", 1, 1},
            {"cos", 1, 1},
            {"degrees", 1, 1},
            {"e", 0, 0},
            {"exp", 1, 1},
            {"floor", 1, 1},
            {"ln", 1, 1},
            {"log", 1, 1},
            {"pi", 0, 0},
            {"power", 2, 2},
            {"radians", 1, 1},
            {"round", 1, 2},
            {"round_even", 1, 2},
            {"sign", 1, 1},
            {"sin", 1, 1},
            {"sqrt", 1, 1},
            {"tan", 1, 1},
            {"trunc", 1, 2},
            {"div", 2, 2},
            {"idiv", 2, 2},

            // Patterns:
            {"regexp_contains", 2, 2},
            {"regexp_like", 2, 2},
            {"regexp_position", 2, 2},
            {"regexp_replace", 3, 9},
            {"fl_like", 2, 2, nullslice, false, true},

            // Strings:
            {"concat", 2, 9},
            {"contains", 2, 2, nullslice, false, true},
            {"length", 1, 1, "N1QL_length"},
            {"lower", 1, 1, "N1QL_lower"},
            {"ltrim", 1, 2, "N1QL_ltrim"},
            {"rtrim", 1, 2, "N1QL_rtrim"},
            {"trim", 1, 2, "N1QL_trim"},
            {"upper", 1, 1, "N1QL_upper"},

            // Types:
            {"isarray", 1, 1},
            {"is_array", 1, 1, "isarray"},
            {"isatom", 1, 1},
            {"is_atom", 1, 1, "isatom"},
            {"isboolean", 1, 1},
            {"is_boolean", 1, 1, "isboolean"},
            {"isnumber", 1, 1},
            {"is_number", 1, 1, "isnumber"},
            {"isobject", 1, 1},
            {"is_object", 1, 1, "isobject"},
            {"isstring", 1, 1},
            {"is_string", 1, 1, "isstring"},
            {"type", 1, 1},
            {"typename", 1, 1, "type"},
            {"toarray", 1, 1},
            {"to_array", 1, 1, "toarray"},
            {"toatom", 1, 1},
            {"to_atom", 1, 1, "toatom"},
            {"toboolean", 1, 1},
            {"to_boolean", 1, 1, "toboolean"},
            {"tonumber", 1, 1},
            {"to_number", 1, 1, "tonumber"},
            {"toobject", 1, 1},
            {"to_object", 1, 1, "toobject"},
            {"tostring", 1, 1},
            {"to_string", 1, 1, "tostring"},
            {"is valued", 1, 1, "isvalued"},

            // FTS (not standard N1QL):
            {"match", 2, 2},
            {"rank", 1, 1},

            // Aggregate functions:
            {"avg", 1, 1, nullslice, true},
            {"count", 0, 1, nullslice, true},
            {"max", 1, 1, nullslice, true},
            {"min", 1, 1, nullslice, true},
            {"sum", 1, 1, nullslice, true},

#ifdef COUCHBASE_ENTERPRISE
            // Predictive query:
            {"prediction", 2, 3},
            {"euclidean_distance", 2, 3},
            {"cosine_distance", 2, 2},

            // Vector search:
            {"approx_vector_dist", 2, 4},
#endif

            {nullslice}  // End of data
    };


    enum JoinType { kInvalidJoin = -1, kInner = 0, kLeft, kLeftOuter, kCross };

    inline static constexpr const char* kJoinTypeNames[] = {
            "INNER", "LEFT", "LEFT OUTER", "CROSS",
            nullptr  // End of data
    };

}  // namespace litecore
