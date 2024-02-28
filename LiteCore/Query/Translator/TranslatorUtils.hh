//
// TranslatorUtils.hh
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
#include "TranslatorTables.hh"
#include "fleece/Fleece.hh"

namespace litecore::qt {
    using namespace fleece;

    class AliasedNode;

    /// Throws an InvalidQuery exception with a formatted message.
    [[noreturn]] __printflike(1, 2) void fail(const char* format, ...);

    /// Throws an InvalidQuery exception if `TEST` is not "truthy".
#define require(TEST, FORMAT, ...)                                                                                     \
    if ( TEST )                                                                                                        \
        ;                                                                                                              \
    else                                                                                                               \
        fail(FORMAT, ##__VA_ARGS__)

    /// Returns the input `val`, but throws an exception if it's not "truthy".
    template <class T>
    static T required(T val, const char* name, const char* message = "is missing") {
        require(val, "%s %s", name, message);
        return val;
    }

    /// Returns `v` as an Array, throwing an exception if it's the wrong type or nullptr.
    Array requiredArray(Value v, const char* what);

    /// Returns `v` as a Dict, throwing an exception if it's the wrong type or nullptr.
    Dict  requiredDict(Value v, const char* what);

    /// Returns `v` as a string (slice), throwing an exception if it's the wrong type or nullptr,
    /// or empty.
    slice requiredString(Value v, const char* what);

    /// Same as `requiredString` but allows `v` to be `nullptr`.
    slice optionalString(Value v, const char* what);

    /// Case insensitive Dict lookup.
    Value getCaseInsensitive(Dict dict, slice key);

    // These functions look up items in tables or convert strings to enums:
    const Operation* lookupOp(slice opName, unsigned nArgs);
    const Operation& lookupOp(OpType type);
    FunctionSpec const& lookupFn(slice fnName, int nArgs);
    MetaProperty lookupMeta(slice key, slice const keyList[kNumMetaProperties]);
    JoinType lookupJoin(slice name);

    /// Common path parsing shared by multiple node types.
    /// `pathStr` may be empty or contain dot-delimited path components;
    /// `pComponents` if given is an array of path components (strings or ints).
    KeyPath parsePath(slice pathStr, fleece::Array::iterator* pComponents = nullptr);

    /// Matches a path's initial component(s) against an alias; if so, drops those component(s) and
    /// returns the source.
    /// If it doesn't match, leaves the path alone and returns `ctx.from`,
    /// which may be nullptr if only an expression is being parsed.
    AliasedNode* resolvePropertyPath(KeyPath&, ParseContext&, bool ignoreJoins = false);

    /// Writes a SQLite function call, passing the given expression.
    /// - If `expr` is a PropertyNode, it writes the node but substitutes the given function name
    ///   for the default `fl_value`.
    /// - Otherwise it writes the function call, passing the value of `expr` as the first arg.
    void writeFnGetter(slice sqliteFnName, ExprNode& expr, ExprNode* param, SQLWriter&);
}
