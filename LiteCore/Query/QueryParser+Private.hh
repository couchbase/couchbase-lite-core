//
//  QueryParser+Private.hh
//  LiteCore
//
//  Copyright © 2018 Couchbase. All rights reserved.
//

#pragma once
#include "QueryParser.hh"
#include "StringUtil.hh"
#include "fleece/slice.hh"
#include "function_ref.hh"
#include <iomanip>

namespace litecore::qp {

    using namespace std;
    using namespace fleece; // enables ""_sl syntax
    using namespace fleece::impl;


#pragma mark - CONSTANTS:

    // Magic property names to reference doc metadata:
    constexpr slice kDocIDProperty        = "_id"_sl;
    constexpr slice kSequenceProperty     = "_sequence"_sl;
    constexpr slice kDeletedProperty      = "_deleted"_sl;
    constexpr slice kExpirationProperty   = "_expiration"_sl;
    constexpr slice kRevIDProperty        = "_revisionID"_sl;


    // Names of the SQLite functions we register for working with Fleece data,
    // in SQLiteFleeceFunctions.cc:
    constexpr slice kValueFnName = "fl_value"_sl;
    constexpr slice kNestedValueFnName = "fl_nested_value"_sl;
    constexpr slice kUnnestedValueFnName = "fl_unnested_value"_sl;
    constexpr slice kFTSValueFnName = "fl_fts_value"_sl;
    constexpr slice kBlobFnName = "fl_blob"_sl;
    constexpr slice kRootFnName  = "fl_root"_sl;
    constexpr slice kEachFnName  = "fl_each"_sl;
    constexpr slice kCountFnName = "fl_count"_sl;
    constexpr slice kExistsFnName= "fl_exists"_sl;
    constexpr slice kResultFnName= "fl_result"_sl;
    constexpr slice kBoolResultFnName = "fl_boolean_result"_sl;
    constexpr slice kContainsFnName = "fl_contains"_sl;
    constexpr slice kNullFnName = "fl_null"_sl;
    constexpr slice kBoolFnName = "fl_bool"_sl;
    constexpr slice kArrayFnNameWithParens = "array_of()"_sl;
    constexpr slice kDictFnName = "dict_of"_sl;
    constexpr slice kVersionFnName  = "fl_version"_sl;

    // Existing SQLite FTS rank function:
    constexpr slice kRankFnName  = "rank"_sl;

    constexpr slice kArrayCountFnName = "array_count"_sl;

    constexpr slice kPredictionFnName = "prediction"_sl;
    constexpr slice kPredictionFnNameWithParens = "prediction()"_sl;

    const char* const kDefaultTableAlias = "_doc";


#pragma mark - FUNCTIONS:

    [[noreturn]] __printflike(1, 2) void fail(const char *format, ...);

    #define require(TEST, FORMAT, ...)  if (TEST) ; else fail(FORMAT, ##__VA_ARGS__)

    template <class T>
    static T required(T val, const char *name, const char *message = "is missing") {
        require(val, "%s %s", name, message);
        return val;
    }

    const Value* getCaseInsensitive(const Dict *dict, slice key);
    bool isImplicitBool(const Value* op);

    const Array* requiredArray(const Value *v, const char *what);
    const Dict* requiredDict(const Value *v, const char *what);
    slice requiredString(const Value *v, const char *what);
    slice optionalString(const Value *v, const char *what);

    Path propertyFromOperands(ArrayIterator &operands, bool skipDot =false);
    Path propertyFromNode(const Value *node, char prefix ='.');

    unsigned findNodes(const Value *root, fleece::slice op, unsigned argCount,
                       function_ref<void(const Array*)> callback);

#pragma mark - FORMATTING:

    bool isAlphanumericOrUnderscore(slice str);
    bool isValidIdentifier(slice str);

    // Temporary wrapper for a slice/string, which adds quotes/escapes when written to an ostream.
    template <char Q, char E>
    struct quotedSlice {
        explicit quotedSlice(slice s) :_raw(s) { }
        quotedSlice(const quotedSlice&) = delete;
        quotedSlice(quotedSlice&&) = delete;

        slice const _raw;
    };

    template <char Q, char E>
    std::ostream& operator<< (std::ostream &out, const quotedSlice<Q,E> &str)  {
        // SQL strings ('') are always quoted; identifiers ("") only when necessary.
        if (Q == '"' && isValidIdentifier(str._raw))
            out.write((const char*)str._raw.buf, str._raw.size);
        else
            out << std::quoted(string_view(str._raw), Q, E);
        return out;
    }

    // Wrap around a string when writing to a stream, to single-quote it as a SQL string literal.
    static inline auto sqlString(slice str) {
        return quotedSlice<'\'','\''>(str);
    }

    // Wrap around a SQL identifier when writing to a stream, to double-quote it if necessary.
    static inline auto sqlIdentifier(slice name) {
        return quotedSlice<'"','"'>(name);
    }

}
