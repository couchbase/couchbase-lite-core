//
// TranslatorTables.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "ExprNodes.hh"

namespace litecore::qt {
    using namespace fleece;


    constexpr slice kDefaultCollectionName = "_default";
    constexpr slice kDefaultScopeName      = "_default";


#pragma mark - META:


    // indexes correspond to MetaProperty, skipping .none
    constexpr slice kMetaPropertyNames[kNumMetaProperties] = {"id",         "sequence",   "deleted",
                                                              "expiration", "revisionID", "rowid"};
    constexpr slice kMetaShortcutNames[kNumMetaProperties] = {"_id",         "_sequence",   "_deleted",
                                                              "_expiration", "_revisionID", "_rowid"};
    constexpr slice kMetaSQLiteNames[kNumMetaProperties]   = {"key",        "sequence", nullslice,
                                                              "expiration", nullslice,  "rowid"};

    // indexed by MetaProperty+1 since it covers _notDeleted
    constexpr OpFlags kMetaFlags[kNumMetaProperties + 2] = {
            kOpBoolResult,    // _notDeleted (-1)
            kOpNoFlags,       // none (0)
            kOpStringResult,  // id
            kOpNumberResult,  // sequence
            kOpBoolResult,    // deleted
            kOpNumberResult,  // expiration
            kOpStringResult,  // revisionID
            kOpNumberResult,  // rowid
    };


#pragma mark - OPERATIONS:


    // This table defines the operations and their characteristics.
    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
    // https://docs.couchbase.com/server/current/n1ql/n1ql-language-reference/index.html
    // http://www.sqlite.org/lang_expr.html

    enum class OpType {
        prefix,   // Prefix operators not specifically named below
        infix,    // Infix operators not specifically named below
        postfix,  // Postfix operators not specifically named below

        property,
        parameter,
        variable,
        objectProperty,
        arrayLiteral,
        blob,
        missing,
        concat,
        is,
        is_not,
        in,
        not_in,
        like,
        between,
        exists,
        isValued,
        collate,
        Case,
        any,
        every,
        anyAndEvery,
        meta,
        select,
        match,
        rank,
#ifdef COUCHBASE_ENTERPRISE
        vectorDistance,
        prediction,
#endif
    };

    constexpr int kArgListPrecedence = -2;  // Prededence inside of `(...., ....)`
    constexpr int kSelectPrecedence  = 1;
    constexpr int kAndPrecedence     = 2;
    constexpr int kMatchPrecedence   = 3;
    constexpr int kCollatePrecedence = 10;
    constexpr int kFnPrecedence      = 99;

    struct Operation {             // NOLINT(cppcoreguidelines-pro-type-member-init)
        slice   name;              // Name, as found in 1st item of array
        int     minArgs, maxArgs;  // Min/max number of args; max 9 means "unlimited"
        int     precedence;        // Precedence in SQLite syntax; used to minimize generated parens
        OpType  type;              // Type of operator
        OpFlags flags;             // Flags, mostly about the result type
    };

    // clang-format off

    constexpr Operation kOperationList[] = {
        {".",               0, 9,  99, OpType::property},
        {"$",               1, 1,  99, OpType::parameter},
        {"?",               1, 9,  99, OpType::variable},
        {"_.",              1, 2,  99, OpType::objectProperty},
        {"[]",              0, 9,  99, OpType::arrayLiteral},
        {"BLOB",            1, 1,  99, OpType::blob},

        {"MISSING",         0, 0,  99, OpType::missing},

        {"||",              2, 9,   3, OpType::concat,      kOpStringResult},

        {"*",               2, 9,   7, OpType::infix,       kOpNumberResult},
        {"/",               2, 2,   7, OpType::infix,       kOpNumberResult},
        {"%",               2, 2,   7, OpType::infix,       kOpNumberResult},

        {"+",               2, 9,   6, OpType::infix,       kOpNumberResult},
        {"-",               2, 2,   6, OpType::infix,       kOpNumberResult},
        {"-",               1, 1,   9, OpType::prefix,      kOpNumberResult},

        {"<",               2, 2,   4, OpType::infix,       kOpBoolResult},
        {"<=",              2, 2,   4, OpType::infix,       kOpBoolResult},
        {">",               2, 2,   4, OpType::infix,       kOpBoolResult},
        {">=",              2, 2,   4, OpType::infix,       kOpBoolResult},

        {"=",               2, 2,   3, OpType::infix,       kOpBoolResult},
        {"!=",              2, 2,   3, OpType::infix,       kOpBoolResult},
        {"IS",              2, 2,   3, OpType::is,          kOpBoolResult},
        {"IS NOT",          2, 2,   3, OpType::is_not,      kOpBoolResult},
        {"IN",              2, 9,   3, OpType::in,          kOpBoolResult},
        {"LIKE",            2, 3,   3, OpType::like,        kOpBoolResult},
        {"NOT IN",          2, 9,   3, OpType::not_in,      kOpBoolResult},
        {"BETWEEN",         3, 3,   3, OpType::between,     kOpBoolResult},
        {"EXISTS",          1, 1,   8, OpType::exists,      kOpBoolResult},
        {"IS VALUED",       1, 1,   3, OpType::isValued,    kOpBoolResult},

        {"NOT",             1, 1,   9, OpType::prefix,      kOpBoolResult},
        {"AND",             2, 9,   2, OpType::infix,       kOpBoolResult},
        {"OR",              2, 9,   2, OpType::infix,       kOpBoolResult},

        {"ANY",             3, 3,   1, OpType::any,         kOpBoolResult},
        {"EVERY",           3, 3,   1, OpType::every,       kOpBoolResult},
        {"ANY AND EVERY",   3, 3,   1, OpType::anyAndEvery, kOpBoolResult},

        {"CASE",            3, 9,   2, OpType::Case},

        {"META()",          0, 1,  kFnPrecedence,           OpType::meta},
        {"MATCH()",         2, 2,  kFnPrecedence,           OpType::match},
        {"RANK()",          1, 1,  kFnPrecedence,           OpType::rank},
        {"COLLATE",         2, 2,  kCollatePrecedence,      OpType::collate},

        {"SELECT",          1, 1,  kSelectPrecedence,       OpType::select},

#ifdef COUCHBASE_ENTERPRISE
        {"APPROX_VECTOR_DISTANCE()", 2, 5, kFnPrecedence,   OpType::vectorDistance},
        {"PREDICTION()",    2, 3, kFnPrecedence,            OpType::prediction},
#endif
    };

    // clang-format on

#pragma mark - FUNCTION NAMES:

    // Names of the SQLite functions we register for working with Fleece data,
    // in SQLiteFleeceFunctions.cc:
    constexpr slice kValueFnName         = "fl_value";
    constexpr slice kNestedValueFnName   = "fl_nested_value";
    constexpr slice kUnnestedValueFnName = "fl_unnested_value";
    constexpr slice kFTSValueFnName      = "fl_fts_value";
    constexpr slice kVectorValueFnName   = "fl_vector_value";
    constexpr slice kVectorToIndexFnName = "fl_vector_to_index";
    constexpr slice kEncodeVectorFnName  = "encode_vector";
    constexpr slice kBlobFnName          = "fl_blob";
    constexpr slice kRootFnName          = "fl_root";
    constexpr slice kEachFnName          = "fl_each";
    constexpr slice kCountFnName         = "fl_count";
    constexpr slice kExistsFnName        = "fl_exists";
    constexpr slice kResultFnName        = "fl_result";
    constexpr slice kBoolResultFnName    = "fl_boolean_result";
    constexpr slice kContainsFnName      = "fl_contains";
    constexpr slice kNullFnName          = "fl_null";
    constexpr slice kBoolFnName          = "fl_bool";
    constexpr slice kVersionFnName       = "fl_version";
    constexpr slice kLikeFnName          = "fl_like";


#pragma mark - N1QL FUNCTIONS:


    // Functions that are emitted by the translator itself:
    constexpr slice kArrayCountFnName     = "array_count";
    constexpr slice kArrayOfFnName        = "array_of";
    constexpr slice kConcatFnName         = "concat";
    constexpr slice kIsValuedFnName       = "is valued";
    constexpr slice kPredictionFnName     = "prediction";
    constexpr slice kVectorDistanceFnName = "approx_vector_distance";

    // Table of functions. Used when the 1st item of the JSON array ends with "()",
    // except for a few special functions declared above in kOperationList.
    // https://developer.couchbase.com/documentation/server/current/n1ql/n1ql-language-reference/functions.html
    // http://www.sqlite.org/lang_corefunc.html
    // http://www.sqlite.org/lang_aggfunc.html

    struct FunctionSpec {          // NOLINT(cppcoreguidelines-pro-type-member-init)
        slice   name;              // Name (without the parens)
        int     minArgs, maxArgs;  // Min/max number of args; max 9 means "unlimited"
        slice   sqlite_name;       // Name to use in SQL; defaults to `name`
        OpFlags flags;             // Flags, mostly about the result type
    };

    // clang-format off

    constexpr FunctionSpec kFunctionList[] = {
        // Array:
        {"array_agg",           1, 1, {},           kOpAggregate},
        {"array_avg",           1, 1, {},           kOpNumberResult},
        {"array_contains",      2, 2, {},           kOpBoolResult},
        {"array_count",         1, 1, {},           kOpNumberResult},
        {"array_ifnull",        1, 1},
        {"array_length",        1, 1, {},           kOpNumberResult},
        {"array_max",           1, 1, {},           kOpNumberResult},
        {"array_min",           1, 1, {},           kOpNumberResult},
        {"array_of",            0, 9},
        {"array_sum",           1, 1, {},           kOpNumberResult},

        // Comparison:  (SQLite min and max are used in non-aggregate form here)
        {"greatest",            2, 9, "max"},
        {"least",               2, 9, "min"},

        // Conditional (unknowns):
        {"ifmissing",           2, 9, "coalesce"},
        {"ifnull",              2, 9, "N1QL_ifnull"},
        {"ifmissingornull",     2, 9},
        {"missingif",           2, 2},
        {"nullif",              2, 2, "N1QL_nullif"},

        // Dates/times:
        {"millis_to_str",       1, 2, {},           kOpStringResult},
        {"millis_to_utc",       1, 2, {},           kOpStringResult},
        {"millis_to_tz",        2, 3, {},           kOpStringResult},
        {"str_to_millis",       1, 1, {},           kOpNumberResult},
        {"str_to_utc",          1, 2, {},           kOpStringResult},
        {"date_diff_str",       3, 3, {},           kOpNumberResult},
        {"date_diff_millis",    3, 3, {},           kOpNumberResult},
        {"date_add_str",        3, 4, {},           kOpStringResult},
        {"date_add_millis",     3, 3, {},           kOpStringResult},
        {"str_to_tz",           2, 3, {},           kOpStringResult},

        // Math:
        {"abs",                 1, 1, {},           kOpNumberResult},
        {"acos",                1, 1, {},           kOpNumberResult},
        {"asin",                1, 1, {},           kOpNumberResult},
        {"atan",                1, 1, {},           kOpNumberResult},
        {"atan2",               2, 2, {},           kOpNumberResult},
        {"ceil",                1, 1, {},           kOpNumberResult},
        {"cos",                 1, 1, {},           kOpNumberResult},
        {"degrees",             1, 1, {},           kOpNumberResult},
        {"e",                   0, 0, {},           kOpNumberResult},
        {"exp",                 1, 1, {},           kOpNumberResult},
        {"floor",               1, 1, {},           kOpNumberResult},
        {"ln",                  1, 1, {},           kOpNumberResult},
        {"log",                 1, 1, {},           kOpNumberResult},
        {"pi",                  0, 0, {},           kOpNumberResult},
        {"power",               2, 2, {},           kOpNumberResult},
        {"radians",             1, 1, {},           kOpNumberResult},
        {"round",               1, 2, {},           kOpNumberResult},
        {"round_even",          1, 2, {},           kOpNumberResult},
        {"sign",                1, 1, {},           kOpNumberResult},
        {"sin",                 1, 1, {},           kOpNumberResult},
        {"sqrt",                1, 1, {},           kOpNumberResult},
        {"tan",                 1, 1, {},           kOpNumberResult},
        {"trunc",               1, 2, {},           kOpNumberResult},
        {"div",                 2, 2, {},           kOpNumberResult},
        {"idiv",                2, 2, {},           kOpNumberResult},

        // Patterns:
        {"regexp_contains",     2, 2, {},           kOpBoolResult},
        {"regexp_like",         2, 2, {},           kOpBoolResult},
        {"regexp_position",     2, 2, {},           kOpNumberResult},
        {"regexp_replace",      3, 9},
        {"fl_like",             2, 2, {},           OpFlags(kOpBoolResult | kOpWantsCollation)},

        // Strings:
        {"concat",              2, 9, {},           kOpStringResult},
        {"contains",            2, 2, {},           OpFlags(kOpBoolResult | kOpWantsCollation)},
        {"length",              1, 1, "N1QL_length",kOpNumberResult},
        {"lower",               1, 1, "N1QL_lower", kOpNumberResult},
        {"ltrim",               1, 2, "N1QL_ltrim", kOpStringResult},
        {"rtrim",               1, 2, "N1QL_rtrim", kOpStringResult},
        {"trim",                1, 2, "N1QL_trim",  kOpStringResult},
        {"upper",               1, 1, "N1QL_upper", kOpStringResult},

        // Types:
        {"isarray",             1, 1, {},           kOpBoolResult},
        {"is_array",            1, 1, "isarray",    kOpBoolResult},
        {"isatom",              1, 1, {},           kOpBoolResult},
        {"is_atom",             1, 1, "isatom",     kOpBoolResult},
        {"isboolean",           1, 1, {},           kOpBoolResult},
        {"is_boolean",          1, 1, "isboolean",  kOpBoolResult},
        {"isnumber",            1, 1, {},           kOpBoolResult},
        {"is_number",           1, 1, "isnumber",   kOpBoolResult},
        {"isobject",            1, 1, {},           kOpBoolResult},
        {"is_object",           1, 1, "isobject",   kOpBoolResult},
        {"isstring",            1, 1, {},           kOpBoolResult},
        {"is_string",           1, 1, "isstring",   kOpBoolResult},
        {"type",                1, 1, {},           kOpStringResult},
        {"typename",            1, 1, "type",       kOpStringResult},
        {"toarray",             1, 1},
        {"to_array",            1, 1, "toarray"},
        {"toatom",              1, 1},
        {"to_atom",             1, 1, "toatom"},
        {"toboolean",           1, 1, {},           kOpBoolResult},
        {"to_boolean",          1, 1, "toboolean",  kOpBoolResult},
        {"tonumber",            1, 1, {},           kOpNumberResult},
        {"to_number",           1, 1, "tonumber",   kOpNumberResult},
        {"toobject",            1, 1},
        {"to_object",           1, 1, "toobject"},
        {"tostring",            1, 1, {},           kOpStringResult},
        {"to_string",           1, 1, "tostring",   kOpStringResult},
        {"is valued",           1, 1, "isvalued",   kOpBoolResult},

        // Aggregate functions:
        {"avg",                 1, 1, {},           OpFlags(kOpNumberResult | kOpAggregate)},
        {"count",               0, 1, {},           OpFlags(kOpNumberResult | kOpAggregate)},
        {"max",                 1, 1, {},           kOpAggregate},
        {"min",                 1, 1, {},           kOpAggregate},
        {"sum",                 1, 1, {},           OpFlags(kOpNumberResult | kOpAggregate)},

#ifdef COUCHBASE_ENTERPRISE
        // Predictive query:
        {"prediction",          2, 3},
        {"euclidean_distance",  2, 3, {},           kOpNumberResult},
        {"cosine_distance",     2, 2, {},           kOpNumberResult},

        // Vector search:
        {"approx_vector_distance", 2, 5, {},        kOpNumberResult},
#endif
    };


    constexpr FunctionSpec kDictOfFunctionSpec      {"dict_of",         0, 9, "dict_of"};
    constexpr FunctionSpec kNestedValueFunctionSpec {"fl_nested_value", 2, 2, "fl_nested_value"};

    // clang-format on

#pragma mark - JOINS:


    // indexed by JoinType enum, SKIPPING .none
    static constexpr const char* kJoinTypeNames[] = {
            "INNER",
            "LEFT",
            "LEFT OUTER",
            "CROSS",
    };

}  // namespace litecore::qt
