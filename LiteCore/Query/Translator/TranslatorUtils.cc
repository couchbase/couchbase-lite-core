//
// TranslatorUtils.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "TranslatorUtils.hh"
#include "Error.hh"
#include "ExprNodes.hh"
#include "Logging.hh"
#include "SelectNodes.hh"
#include "StringUtil.hh"
#include "SQLWriter.hh"
#include <climits>

namespace litecore::qt {

    void fail(const char* format, ...) {
        va_list args;
        va_start(args, format);
        string message = vformat(format, args);
        va_end(args);

        Warn("Invalid LiteCore query: %s", message.c_str());
        throw error(error::LiteCore, error::InvalidQuery, message);
    }

    Array requiredArray(Value v, const char* what) {
        return required(required(v, what).asArray(), what, "must be an array");
    }

    Dict requiredDict(Value v, const char* what) {
        return required(required(v, what).asDict(), what, "must be a dictionary");
    }

    slice requiredString(Value v, const char* what) {
        slice str = required(required(v, what).asString(), what, "must be a string");
        require(str.size > 0, "%s must be non-empty", what);
        return str;
    }

    slice optionalString(Value v, const char* what) {
        slice str;
        if ( v ) {
            str = required(v.asString(), what, "must be a string");
            require(str.size > 0, "%s must be non-empty", what);
        }
        return str;
    }

    Value getCaseInsensitive(Dict dict, slice key) {
        for ( Dict::iterator i(dict); i; ++i )
            if ( i.keyString().caseEquivalent(key) ) return i.value();
        return nullptr;
    }

#pragma mark - TABLE LOOKUP:

    const Operation* lookupOp(slice opName, unsigned nArgs) {
        bool nameMatched = false;
        for ( auto& def : kOperationList ) {
            if ( opName.caseEquivalent(def.name) ) {
                nameMatched = true;
                if ( nArgs >= def.minArgs && nArgs <= def.maxArgs ) return &def;
            }
        }
        if ( nameMatched ) fail("Wrong number of arguments to %.*s", FMTSLICE(opName));
        return nullptr;
    }

    const Operation& lookupOp(OpType type) {
        for ( auto& def : kOperationList ) {
            if ( def.type == type ) return def;
        }
        fail("Internal error: No Operation with type %d", int(type));
    }

    FunctionSpec const& lookupFn(slice fnName, int nArgs) {
        bool nameMatched = false;
        for ( auto& def : kFunctionList ) {
            if ( fnName.caseEquivalent(def.name) ) {
                nameMatched = true;
                if ( nArgs >= def.minArgs && nArgs <= def.maxArgs ) return def;
            }
        }
        if ( nameMatched ) fail("Wrong number of arguments to %.*s()", FMTSLICE(fnName));
        else
            fail("Unknown function '%.*s'", FMTSLICE(fnName));
    }

    MetaProperty lookupMeta(slice key, slice const keyList[kNumMetaProperties]) {
        for ( int i = 0; i < kNumMetaProperties; ++i ) {
            if ( 0 == key.caseEquivalentCompare(keyList[i]) ) return MetaProperty{i + 1};
        }
        return MetaProperty::none;
    }

    JoinType lookupJoin(slice name) {
        int i = 0;
        for ( auto j : kJoinTypeNames ) {
            if ( name.caseEquivalent(j) ) return JoinType{i};
            ++i;
        }
        return JoinType::none;
    }

#pragma mark - PATHS:

    KeyPath parsePath(slice pathStr, fleece::Array::iterator* pComponents) {
        KeyPath path;
        if ( !pathStr.empty() ) {
            FLError error;
            if ( pathStr.hasPrefix("$") ) {
                // JSONPath prefix ignores a leading '$', but in query syntax it's treated literally;
                // so escape it before passing it to the parser:
                string quoted(pathStr);
                quoted.insert(quoted.begin(), '\\');
                path = KeyPath(quoted, &error);
            } else {
                path = KeyPath(pathStr, &error);
            }
            require(path, "invalid property path '%.*s'", FMTSLICE(pathStr));
        }
        if ( pComponents ) {
            for ( Array::iterator& i = *pComponents; i; ++i ) {
                if ( slice key = i->asString() ) {
                    path.addProperty(key);
                } else {
                    auto arr = i->asArray();
                    require(arr, "Invalid JSON value in property path");
                    require(arr.count() == 1, "Property array index must have exactly one item");
                    Value indexVal = arr[0];
                    require(indexVal.isInteger(), "Property array index must be an integer");
                    int64_t index = indexVal.asInt();
                    require(index >= INT_MIN && index <= INT_MAX, "array index out of bounds in property path");
                    path.addIndex(int(index));
                }
            }
        }
        return path;
    }

    static bool hasMultipleCollections(ParseContext& ctx, bool ignoreJoins) {
        if ( ctx.sources.size() >= 2 ) {
            if ( ignoreJoins ) {
                string const& collection = ctx.from->collection();
                string const& scope      = ctx.from->scope();
                for ( auto source : ctx.sources ) {
                    if ( source->collection() != collection || source->scope() != scope ) return true;
                }
            } else {
                return true;
            }
        }
        return false;
    }

    AliasedNode* resolvePropertyPath(KeyPath& path, ParseContext& ctx, bool ignoreJoins) {
        // First check whether the path starts with an alias; if so use it as the source:
        for ( auto& a : ctx.aliases ) {
            if ( a.second->matchPath(path) ) return a.second;
        }

        if ( path.count() >= 1 && ctx.from && !ctx.from->hasExplicitAlias() ) {
            // As a special case, we'll match on just the collection name of the main source,
            // even if it has a scope name:
            slice first = path.get(0).first;
            DebugAssert(!first.empty());
            if ( first.caseEquivalent(ctx.from->collection()) ) {
                path.dropComponents(1);
                return ctx.from;
            }
        }

        // If there are no JOINs, the property is implicitily on the main (FROM) source.
        if ( hasMultipleCollections(ctx, ignoreJoins) ) {
            alloc_slice pathStr = path.toString();
            fail("property '%.*s' does not begin with a declared 'AS' alias", FMTSLICE(pathStr));
        }
        return ctx.from;  // note: may be nullptr if parsing just an expression
    }

    void writeFnGetter(slice sqliteFnName, ExprNode& expr, ExprNode* param, SQLWriter& ctx) {
        if ( auto prop = dynamic_cast<PropertyNode*>(&expr) ) {
            prop->writeSQL(ctx, sqliteFnName, param);
        } else {
            ctx << sqliteFnName << '(' << expr;
            if ( param ) ctx << ", NULL, " << *param;
            ctx << ')';
        }
    }

}  // namespace litecore::qt
