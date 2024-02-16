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

    [[noreturn]] __printflike(1, 2) void fail(const char* format, ...);

#define require(TEST, FORMAT, ...)                                                                                     \
    if ( TEST )                                                                                                        \
        ;                                                                                                              \
    else                                                                                                               \
        fail(FORMAT, ##__VA_ARGS__)

    template <class T>
    static T required(T val, const char* name, const char* message = "is missing") {
        require(val, "%s %s", name, message);
        return val;
    }

    Value getCaseInsensitive(Dict dict, slice key);
    bool  isImplicitBool(Value op);

    Array requiredArray(Value v, const char* what);
    Dict  requiredDict(Value v, const char* what);
    slice requiredString(Value v, const char* what);
    slice optionalString(Value v, const char* what);

    alloc_slice escapedPath(slice inputPath);
    const Operation* lookupOp(slice opName, unsigned nArgs);
    const Operation& lookupOp(OpType type);
    FunctionSpec const& lookupFn(slice fnName, int nArgs);
    MetaProperty lookupMeta(slice key, slice const keyList[kNumMetaProperties]);
    JoinType lookupJoin(slice name);

    KeyPath parsePath(slice pathStr, fleece::Array::iterator* pComponents = nullptr);

    AliasedNode* resolvePropertyPath(KeyPath& path, ParseContext ctx,
                                     bool ignoreJoins = false);

    void writeFnGetter(slice sqliteFnName, ExprNode& collection, ExprNode* param, SQLWriter&);
}
