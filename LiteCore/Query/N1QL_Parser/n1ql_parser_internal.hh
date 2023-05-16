//
//  n1ql_parser_internal.hh
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// This header is included by the generated parser (n1ql.cc),
// so its contents are available to actions in the grammar file (n1ql.leg).

#pragma once
#include "n1ql_parser.hh"
#include "fleece/Mutable.hh"
#include "Any.hh"
#include "PlatformIO.hh"
#include <algorithm>
#include <array>
#include <sstream>
#include <typeinfo>
#include "betterassert.hh"

namespace litecore { namespace n1ql {

using namespace fleece;
using namespace litecore;
using string = std::string;


// Configuration for the 'leg'-generated parser code:


#define YY_CTX_LOCAL
#define YY_CTX_MEMBERS  std::stringstream *stream;

#define YYSTYPE Any     // The data type returned by grammar-rule actions

#define YY_LOCAL(T) LITECORE_UNUSED static T
#define YY_RULE(T)  LITECORE_UNUSED static T
#define YY_PARSE(T) static T

#define YYPARSE     parse
#define YYPARSEFROM n1ql_parse_from

#define YY_INPUT(ctx, buf, result, max_size) ((result)=n1ql_input(ctx, buf, max_size))
static int n1ql_input(struct _yycontext *ctx, char *buf, size_t max_size);


// Adding 'Any' values to Array/Dict:


static MutableDict setAny(MutableDict dict, slice key, const Any &value) {
    if (value.isNull())
        return dict;
    if (value.with<MutableArray>([&](const MutableArray &v)     {dict.set(key, v);})
            || value.with<MutableDict>([&](const MutableDict &v){dict.set(key, v);})
            || value.with<Value>([&](const Value &v)            {dict.set(key, v);})
            || value.with<string>([&](const string &v)          {dict.set(key, v.c_str());})
            || value.with<const char*>([&](const char *v)       {dict.set(key, v);})
            || value.with<long long>([&](const long long &v)    {dict.set(key, (int64_t)v);})
            || value.with<double>([&](const double &v)          {dict.set(key, v);})
            || value.with<bool>([&](const bool &v)              {dict.set(key, v);})
            || value.with<Null>([&](const Null &v)              {dict.set(key, v);}))
        return dict;
    throw std::bad_cast();
}

static MutableArray setAny(MutableArray array, unsigned index, const Any &value) {
    precondition(!value.isNull());
    if (value.with<MutableArray>([&](const MutableArray &v)     {array.set(index, v);})
            || value.with<MutableDict>([&](const MutableDict &v){array.set(index, v);})
            || value.with<Value>([&](const Value &v)            {array.set(index, v);})
            || value.with<string>([&](const string &v)          {array.set(index, v.c_str());})
            || value.with<const char*>([&](const char *v)       {array.set(index, v);})
            || value.with<long long>([&](const long long &v)    {array.set(index, (int64_t)v);})
            || value.with<double>([&](const double &v)          {array.set(index, v);})
            || value.with<bool>([&](const bool &v)              {array.set(index, v);})
            || value.with<Null>([&](const Null &v)              {array.set(index, v);}))
        return array;
    throw std::bad_cast();
}

static MutableArray insertAny(MutableArray array, unsigned index, const Any &value) {
    array.insertNulls(index, 1);
    return setAny(array, index, value);
}

static MutableArray appendAny(MutableArray array, const Any &value) {
    return insertAny(array, array.count(), value);
}


// Constructing arrays:


static inline MutableArray array() {
    return MutableArray::newArray();
}

template <class T>
static MutableArray arrayWith(T item) {
    auto a = array();
    a.append(item);
    return a;
}

template <>
MutableArray arrayWith(Any item) {
    auto a = array();
    return appendAny(a, item);
}

template <class T>
static MutableDict dictWith(slice key, T item) {
    auto d = MutableDict::newDict();
    d.set(key, item);
    return d;
}

template <>
MutableDict dictWith(slice key, Any item) {
    auto d = MutableDict::newDict();
    setAny(d, key, item);
    return d;
}

static MutableArray op(const Any &oper, const Any &op1, const Any &op2);
static MutableArray binaryOp(const Any &left, const Any &oper, const Any &right);

// Constructing JSON operations:


static inline MutableArray op(const Any &oper) {
    return arrayWith(oper);
}

static MutableArray op(const Any &oper, const Any &op1) {
    string postOp;
    if (!oper.with<string>([&postOp](const string& s) {
        postOp = s;
    })) {
        return appendAny(op(oper), op1);
    }

    std::array<const char*, 7> postOps = {{
        "NOT NULL",
        "IS NULL",
        "IS MISSING",
        "IS VALUED",
        "IS NOT NULL",
        "IS NOT MISSING",
        "IS NOT VALUED"
    }};
    auto i = std::find(postOps.begin(), postOps.end(), postOp);
    if (i == postOps.end()) {
        return appendAny(op(oper), op1);
    }
    size_t at = i - postOps.begin();
    switch (at) {
        case 0:
            return op("IS NOT", op1, nullValue);
        case 1:
            return binaryOp(op1, "IS", nullValue);
        case 2:
            return binaryOp(op1, "IS", op("MISSING"));
        case 4:
            return binaryOp(op1, "IS NOT", nullValue);
        case 5:
            return binaryOp(op1, "IS NOT", op("MISSING"));
        case 6:
            return op("NOT", op("IS VALUED", op1));
        case 3:
        default:
            return appendAny(op(oper), op1);
    }
}

static MutableArray op(const Any &oper, const Any &op1, const Any &op2) {
    return appendAny(op(oper, op1), op2);
}

static MutableArray op(const Any &oper, const Any &op1, const Any &op2, const Any &op3) {
    return appendAny(op(oper, op1, op2), op3);
}

static MutableArray binaryOp(const Any &left, const Any &oper, const Any &right) {
    return op(oper, left, right);
}

static MutableArray unaryOp(const Any &oper, const Any &right) {
    return op(oper, right);
}


// String utilities:


static void uppercase(string &str) {
    for (char &c : str)
        c = (char)toupper(c);
}


static void replace(std::string &str, const std::string &oldStr, const std::string &newStr) {
    string::size_type pos = 0;
    while (string::npos != (pos = str.find(oldStr, pos))) {
        str.replace(pos, oldStr.size(), newStr);
        pos += newStr.size();
    }
}

static string trim (const char *input) {
    while (isspace(*input))
        ++input;
    const char *last = input + strlen(input) - 1;
    while (last >= input && isspace(*last))
        --last;
    return string(input, last-input+1);
}

static string unquote(string str, char quoteChar) {
    replace(str, string(2, quoteChar), string(1, quoteChar));
    return str;
}


// Property-path operations:


static string quoteProperty(string prop) {
    replace(prop, ".", "\\.");
    replace(prop, "$", "\\$");
    prop.replace(0, 0, ".");
    return prop;
}

static string concatProperty(const string &prop, const string &prop2) {
    return prop + quoteProperty(prop2);
}

static string concatIndex(const string &prop, long long i) {
    return prop + "[" + std::to_string(i) + "]";
}

static bool hasPathPrefix(slice path, slice prefix) {
    return path.hasPrefix(prefix) && (path.size == prefix.size ||
                                      (path[prefix.size] == '.' || path[prefix.size] == '['));
}


// Variable substitution:


static void _substituteVariable(slice varWithDot, MutableArray expr) {
    int index = 0;
    for (Array::iterator i(expr); i; ++i) {
        Value item = i.value();
        if (index++ == 0) {
            if (hasPathPrefix(item.asString(), varWithDot)) {
                // Change '.xxx' to '?xxx':
                string op = string(item.asString());
                op[0] = '?';
                expr[0] = op.c_str();
            }
        } else {
            auto operation = item.asArray().asMutable();
            if (operation)
                _substituteVariable(varWithDot, operation); // recurse
        }
    }
}

// Postprocess an expression by changing references to 'var' from a property to a variable.
static void substituteVariable(const string &var, MutableArray expr) {
    _substituteVariable(slice("." + var), expr);
}


// Recognizing reserved words & function names:


static const char* kReservedWords[] = {
    "AND",  "ANY",  "AS",  "ASC",  "BETWEEN",  "BY",  "CASE",  "CROSS",  "DESC",  "DISTINCT",
    "ELSE",  "END",  "EVERY",  "FALSE",  "FROM",  "GROUP",  "HAVING",  "IN",  "INNER",  "IS",
    "JOIN",  "LEFT",  "LIKE",  "LIMIT",  "MISSING",  "NATURAL",  "NOT",
    "NULL",  "MISSING",  "OFFSET",  "ON",  "OR",  "ORDER",  "OUTER",  "RIGHT",
    "SATISFIES",  "SELECT",  "THEN",  "TRUE",  "USING",  "VALUED", "WHEN",  "WHERE",
    "COLLATE",
    nullptr
};

static const char* kFunctions[] = {         // (copied from LiteCore's QueryParserTables.hh)
    // Array:
    "array_agg", "array_avg",  "array_contains",  "array_count",  "array_ifnull",  "array_length",  "array_max",
    "array_min",  "array_of",  "array_sum",
    // Comparison:  (SQLite min and max are used in non-aggregate form here)
    "greatest",  "least",
    // Conditional (unknowns):
    "ifmissing",  "ifnull",  "ifmissingornull",  "missingif",  "nullif",
    // Dates/times:
    "millis_to_str",  "millis_to_utc",  "str_to_millis",  "str_to_utc",
    // Math:
    "abs",  "acos",  "asin",  "atan",  "atan2",  "ceil",  "cos",  "degrees",  "e",  "exp",
    "floor",  "ln",  "log",  "pi",  "power",  "radians",  "round", "round_even", "sign",  "sin",  "sqrt",
    "tan",  "trunc", "div", "idiv",
    // Patterns:
    "regexp_contains",  "regexp_like",  "regexp_position",  "regexp_replace",
    // Strings:
    "contains",  "length",  "lower",  "ltrim",  "rtrim",  "trim",  "upper",
    // Types:
    "isarray",  "isatom",  "isboolean",  "isnumber",  "isobject",  "isstring",  "type",  "toarray",
    "toatom",  "toboolean",  "tonumber",  "toobject",  "tostring",
    "is_array",  "is_atom",  "is_boolean",  "is_number",  "is_object",  "is_string",  "typename",  "to_array",
    "to_atom",  "to_boolean",  "to_number",  "to_object",  "to_string",
    // Aggregate functions:
    "avg",  "count",  "max",  "min",  "sum",
    // Predictive query:
    "prediction",  "euclidean_distance",  "cosine_distance",
    nullptr
};

static bool findIdentifier(const char *ident, const char* list[]) {
    for (int i = 0; list[i]; ++i)
        if (strcasecmp(ident, list[i]) == 0)
            return true;
    return false;
}

static inline bool isReservedWord(const char *ident) {
    return findIdentifier(ident, kReservedWords);
}

static inline bool isFunction(const char *fn) {
    return findIdentifier(fn, kFunctions);
}


// Collation modes:


static void extendCollate(MutableArray expr, string collation) {
    auto coll = expr[1].asDict().asMutable();
    precondition(coll);
    string colonSuffix; // language code for UNICODE
    if (auto p = collation.find(':'); p != string::npos) {
        colonSuffix = collation.substr(p+1);
        collation = collation.substr(0, p);
    }
    uppercase(collation);
    bool value = (collation.substr(0,2) != "NO");
    if (!value)
        collation = collation.substr(2);
    coll[slice(collation)] = value;
    if (colonSuffix.length() > 0) {
        coll["LOCALE"_sl] = colonSuffix;
    }
}

static MutableArray collateOp(MutableArray expr, string collation) {
    auto collate = op("COLLATE", MutableDict::newDict(), expr);
    extendCollate(collate, collation);
    return collate;
}

} }
