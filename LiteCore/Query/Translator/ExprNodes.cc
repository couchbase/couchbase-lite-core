//
// ExprNodes.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ExprNodes.hh"
#include "IndexedNodes.hh"
#include "SelectNodes.hh"
#include "Error.hh"
#include "SQLUtil.hh"
#include "StringUtil.hh"
#include "TranslatorTables.hh"
#include "TranslatorUtils.hh"
#include <iostream>

namespace litecore::qt {
    using namespace fleece;
    using namespace std;

#pragma mark - EXPRESSION PARSING:

    // Top-level method to parse an expression.
    ExprNode* ExprNode::parse(Value v, ParseContext& ctx) {
        switch ( v.type() ) {
            case kFLUndefined:
                fail("internal error: missing Value for expression");
            case kFLNull:
            case kFLBoolean:
            case kFLNumber:
            case kFLString:
                return new (ctx) LiteralNode(v);
            case kFLData:
                fail("Binary data not supported in query");
            case kFLArray:
                return parseArray(v.asArray(), ctx);
            case kFLDict:
                {
                    // Construct a `dict_of(...)` call:
                    auto result = new (ctx) FunctionNode(kDictOfFunctionSpec);
                    for ( Dict::iterator i(v.asDict()); i; ++i ) {
                        result->addArg(new (ctx) LiteralNode(i.key()));
                        result->addArg(ExprNode::parse(i.value(), ctx));
                    }
                    return result;
                }
        }
    }

    ExprNode* ExprNode::parseArray(Array array, ParseContext& ctx) {
        // The first item of an array is a string, the operation; the rest are operands:
        Array::iterator operands(array);
        slice           opName = requiredString(operands[0], "operation");
        ++operands;
        unsigned nargs = min(operands.count(), 9u);

        if ( Operation const* op = lookupOp(opName, nargs) ) {
            return parseOp(*op, operands, ctx);
        } else if ( opName.hasPrefix('.') ) {
            return PropertyNode::parse(opName.from(1), &operands, ctx);
        } else if ( opName.hasPrefix("_."_sl) ) {
            require(nargs == 1, "expected single arg for '%.*s' object accessor", FMTSLICE(opName));
            auto fn = new (ctx) FunctionNode(kNestedValueFunctionSpec);
            fn->addArg(ExprNode::parse(operands[0], ctx));
            fn->addArg(new (ctx) LiteralNode(opName.from(2)));
            return fn;
        } else if ( opName.hasPrefix('$') ) {
            require(operands.count() == 0, "extra operands to '%.*s'", FMTSLICE(opName));
            return new (ctx) ParameterNode(opName.from(1), ctx);
        } else if ( opName.hasPrefix('?') ) {
            return VariableNode::parse(opName.from(1), operands, ctx);
        } else if ( opName.hasSuffix("()"_sl) ) {
            return FunctionNode::parse(opName.upTo(opName.size - 2), operands, ctx);
        } else {
            fail("Unknown operator '%.*s'", FMTSLICE(opName));
        }
    }

    ExprNode* ExprNode::parseOp(Operation const& op, Array::iterator operands, ParseContext& ctx) {
        switch ( op.type ) {
            case OpType::any:
            case OpType::every:
            case OpType::anyAndEvery:
                return new (ctx) AnyEveryNode(op, operands, ctx);
            case OpType::arrayLiteral:
                {
                    auto fn = new (ctx) FunctionNode(lookupFn(kArrayOfFnName, operands.count()));
                    fn->addArgs(operands, ctx);
                    return fn;
                }
            case OpType::blob:
                return parseBlob(op, operands, ctx);
            case OpType::collate:
                return CollateNode::parse(requiredDict(operands[0], "COLLATE dict"), operands[1], ctx);
            case OpType::concat:
                return FunctionNode::parse(kConcatFnName, operands, ctx);
            case OpType::exists:
                return parseExists(op, operands, ctx);
            case OpType::in:
            case OpType::not_in:
                return parseInNotIn(op, operands, ctx);
            case OpType::isValued:
                return FunctionNode::parse(kIsValuedFnName, operands, ctx);
            case OpType::match:
                return new (ctx) MatchNode(operands, ctx);
            case OpType::meta:
                return new (ctx) MetaNode(operands, ctx);
            case OpType::objectProperty:
                return parseObjectProperty(op, operands, ctx);
            case OpType::property:
                return PropertyNode::parse(nullslice, &operands, ctx);
            case OpType::parameter:
                return new (ctx) ParameterNode(operands[0], ctx);
            case OpType::rank:
                return new (ctx) RankNode(operands, ctx);
            case OpType::select:
                return new (ctx) SelectNode(operands[0], ctx);
            case OpType::variable:
                return VariableNode::parse(nullslice, operands, ctx);
#ifdef COUCHBASE_ENTERPRISE
            case OpType::vectorDistance:
                return new (ctx) VectorDistanceNode(operands, ctx);
#endif
            default:
                // A normal OpNode
                return new (ctx) OpNode(op, operands, ctx);
        }
    }

    ExprNode* ExprNode::parseInNotIn(Operation const& op, Array::iterator& operands, ParseContext& ctx) {
        auto lhs          = parse(operands[0], ctx);
        auto arrayOperand = operands[1].asArray();
        if ( arrayOperand && arrayOperand[0].asString() == "[]"_sl ) {
            // RHS is a literal array, so use SQL "IN" syntax:
            auto result = new (ctx) OpNode(op);
            result->addArg(lhs);
            for ( unsigned i = 1; i < arrayOperand.count(); ++i ) result->addArg(parse(arrayOperand[i], ctx));
            return result;
        } else {
            // Otherwise generate a call to array_contains():
            // [yes, operands are in reverse order]
            auto contains = new (ctx) FunctionNode(lookupFn("array_contains", 2));
            contains->addArg(parse(operands[1], ctx));
            contains->addArg(lhs);
            if ( op.type == OpType::not_in ) {
                auto n = new (ctx) OpNode(*lookupOp("NOT", 1));
                n->addArg(contains);
                return n;
            } else {
                return contains;
            }
        }
    }

    ExprNode* ExprNode::parseExists(Operation const& op, Array::iterator& operands, ParseContext& ctx) {
        ExprNode* arg = parse(operands[0], ctx);
        if ( auto prop = dynamic_cast<PropertyNode*>(arg) ) {
            // "EXISTS propertyname" turns into a call to fl_exists()
            prop->setSQLiteFn(kExistsFnName);
            return arg;
        } else {
            auto exists = new (ctx) OpNode(op);
            exists->addArg(arg);
            return exists;
        }
    }

    ExprNode* ExprNode::parseBlob(Operation const& op, Array::iterator& operands, ParseContext& ctx) {
        ExprNode* arg;
        if ( slice propStr = operands[0].asString() ) {
            arg = PropertyNode::parse(propStr, nullptr, ctx);
        } else {
            arg = parse(operands[0], ctx);
            require(dynamic_cast<PropertyNode*>(arg), "argument of BLOB() must be a document property");
        }
        auto blob = new (ctx) OpNode(op);
        blob->addArg(arg);
        return blob;
    }

    ExprNode* ExprNode::parseObjectProperty(Operation const& op, Array::iterator& operands, ParseContext& ctx) {
        auto node = new (ctx) OpNode(op, operands, ctx);
        // Convert ['_.', ['META()'], 'x']  ->  ['._x'] :
        auto meta = dynamic_cast<MetaNode*>(node->operand(0));
        if ( meta && meta->property() == MetaProperty::none ) {
            if ( auto key = dynamic_cast<LiteralNode*>(node->operand(1)) ) {
                if ( slice keyStr = key->asString() ) {
                    // OK, matched the pattern. Now identify the key:
                    if ( keyStr.hasPrefix('.') ) keyStr.moveStart(1);
                    MetaProperty prop = lookupMeta(keyStr, kMetaPropertyNames);
                    require(prop != MetaProperty::none, "'%.*s' is not a valid Meta key", FMTSLICE(keyStr));
                    return new (ctx) MetaNode(prop, meta->source());
                }
            }
        }
        return node;
    }

#pragma mark - LEAF NODES:

    LiteralNode::LiteralNode(Value v) : _literal(v) { Assert(v.type() < kFLData); }

    LiteralNode::LiteralNode(int64_t i) : _literal(i) {}

    LiteralNode::LiteralNode(string_view str) : _literal(str) {}

    FLValueType LiteralNode::type() const {
        switch ( _literal.index() ) {
            case 0:
                return get<Value>(_literal).type();
            case 1:
                return kFLNumber;
            case 2:
                return kFLString;
            default:
                abort();  // unreachable
        }
    }

    optional<int64_t> LiteralNode::asInt() const {
        switch ( _literal.index() ) {
            case 0:
                if ( Value v = get<Value>(_literal); v.isInteger() ) return v.asInt();
                break;
            case 1:
                return get<int64_t>(_literal);
            default:
                break;
        }
        return nullopt;
    }

    string_view LiteralNode::asString() const {
        switch ( _literal.index() ) {
            case 0:
                return get<Value>(_literal).asString();
            case 2:
                return get<string_view>(_literal);
            default:
                return "";
        }
    }

    void LiteralNode::setInt(int64_t i) { _literal = i; }

    MetaNode::MetaNode(Array::iterator& args, ParseContext& ctx) {
        if ( args.count() == 0 ) {
            _source = ctx.from;
        } else {
            slice   arg  = requiredString(args[0], "meta() argument");
            KeyPath path = parsePath(arg);
            _source      = dynamic_cast<SourceNode*>(resolvePropertyPath(path, ctx));
            require(_source && path.count() == 0, "database alias '%.*s' does not match a declared 'AS' alias",
                    FMTSLICE(arg));
        }
    }

    MetaNode::MetaNode(MetaProperty p, SourceNode* C4NULLABLE src) : _property(p), _source(src) {}

    SourceNode* MetaNode::source() const { return _source; }

    string_view MetaNode::asColumnName() const {
        if ( _property != MetaProperty::none ) {
            return kMetaPropertyNames[int(_property) - 1];
        } else if ( _source ) {
            return _source->asColumnName();
        } else {
            return "";
        }
    }

    OpFlags MetaNode::opFlags() const { return kMetaFlags[int(_property) + 1]; }

    ParameterNode::ParameterNode(string_view name, ParseContext& ctx) : _name(ctx.newString(name)) {
        require(isAlphanumericOrUnderscore(_name), "Invalid query parameter name '%s'", _name);
    }

#pragma mark - PROPERTY NODE:

    ExprNode* PropertyNode::parse(slice pathStr, fleece::Array::iterator* components, ParseContext& ctx) {
        KeyPath     path   = parsePath(pathStr, components);
        SourceNode* source = nullptr;
        WhatNode*   result = nullptr;
        string_view sqliteFn;

        if ( AliasedNode* a = resolvePropertyPath(path, ctx) ) {
            // 1st component of path names a source or result, and has been removed from `path`.
            source = dynamic_cast<SourceNode*>(a);
            if ( !source ) {
                result = dynamic_cast<WhatNode*>(a);
                Assert(result);
            }
        }
        if ( result ) {
            // This property is a result alias, or a child thereof:
            if ( path.count() > 0 ) { sqliteFn = kNestedValueFnName; }
        } else if ( path.count() == 0 ) {
            // Empty path: refers to the root of the source
            sqliteFn = kRootFnName;
        } else {
            slice first = path.get(0).first;
            require(first.size > 0, "property cannot start with an array index");
            MetaProperty meta = lookupMeta(first, kMetaShortcutNames);
            if ( meta != MetaProperty::none ) {
                path.dropComponents(1);
                require(path.count() == 0, "invalid properties after a meta property");
                return new (ctx) MetaNode(meta, source);
            }
            if ( source && source->type() == SourceType::unnest ) sqliteFn = kNestedValueFnName;
            else
                sqliteFn = kValueFnName;
        }

        string_view lastComponent;
        if ( path.count() > 0 ) lastComponent = ctx.newString(path.get(path.count() - 1).first);
        return new (ctx) PropertyNode(source, result, ctx.newString(string(path)), lastComponent, sqliteFn);
    }

    PropertyNode::PropertyNode(SourceNode* C4NULLABLE src, WhatNode* C4NULLABLE result, string_view path,
                               string_view lastComponent, string_view fn)
        : _source(src), _result(result), _path(path), _lastComponent(lastComponent), _sqliteFn(fn) {}

    string_view PropertyNode::asColumnName() const {
        if ( !_path.empty() ) return _lastComponent;
        else if ( _source )
            return _source->asColumnName();
        else
            return "";
    }

    SourceNode* PropertyNode::source() const { return _source; }

#pragma mark - VARIABLE NODE:

    ExprNode* VariableNode::parse(slice pathStr, Array::iterator& args, ParseContext& ctx) {
        if ( !pathStr ) {
            pathStr = requiredString(*args, "variable name");
            ++args;
        }
        KeyPath path = parsePath(pathStr, &args);
        require(path.count() > 0, "invalid variable name");
        slice varName = path.get(0).first;
        require(isValidIdentifier(varName), "Invalid variable name '%.*s'", FMTSLICE(varName));
        auto var = new (ctx) VariableNode(ctx.newString(varName));

        if ( path.count() == 1 ) {
            return var;
        } else {
            // There's a path after the variable name. Expand this to a property access:
            path.dropComponents(1);
            var->_returnBody = true;

            auto expr = new (ctx) OpNode(lookupOp(OpType::objectProperty));
            expr->addArg(var);
            expr->addArg(new (ctx) LiteralNode(ctx.newString(path.toString())));
            return expr;
        }
    }

#pragma mark - COLLATE NODE:

    ExprNode* CollateNode::parse(Dict options, Value childVal, ParseContext& ctx) {
        // a COLLATE op merely changes the current collation.
        // First update the current Collation from the options dict and push it:
        Collation savedCollation        = ctx.collation;
        bool      savedCollationApplied = ctx.collationApplied;

        auto setFlagFromOption = [&](bool& flag, slice key) {
            if ( Value val = getCaseInsensitive(options, key) ) flag = val.asBool();
        };
        setFlagFromOption(ctx.collation.caseSensitive, "CASE");
        setFlagFromOption(ctx.collation.diacriticSensitive, "DIAC");
        setFlagFromOption(ctx.collation.unicodeAware, "UNICODE");
        if ( Value localeName = getCaseInsensitive(options, "LOCALE") )
            ctx.collation.localeName = localeName.asString();
        ctx.collationApplied = false;

        // Parse the child:
        auto node = ExprNode::parse(childVal, ctx);

        if ( !ctx.collationApplied ) {
            // If no nested node used the collation, insert it into the tree here
            // so it will written:
            node = new (ctx) CollateNode(node, ctx);
        }

        // Finally pop the saved Collation:
        ctx.collation        = savedCollation;
        ctx.collationApplied = savedCollationApplied;
        return node;
    }

    CollateNode::CollateNode(ExprNode* child, ParseContext& ctx) : _collation(ctx.collation) {
        initChild(_child, child);
        ctx.collationApplied = true;
    }

    bool CollateNode::isBinary() const { return _collation.caseSensitive && !_collation.unicodeAware; }

    void CollateNode::visitChildren(ChildVisitor const& visitor) { visitor(_child); }

#pragma mark - OP NODE:

    OpNode::OpNode(Operation const& op, Array::iterator& operands, ParseContext& ctx) : OpNode(op) {
        for ( ; operands; ++operands ) addArg(ExprNode::parse(*operands, ctx));

        if ( !ctx.collationApplied && (op.type == OpType::infix || op.type == OpType::like) ) {
            // Apply the current collation by wrapping the first operand in a CollateNode:
            ExprNode* op0 = _operands.pop_front();
            op0->setParent(nullptr);
            auto coll = new (ctx) CollateNode(op0, ctx);
            coll->setParent(this);
            _operands.push_front(coll);
        }
    }

    OpFlags OpNode::opFlags() const { return _op.flags; }

    void OpNode::visitChildren(ChildVisitor const& visitor) { visitor(_operands); }

#if DEBUG
    void OpNode::postprocess(ParseContext& ctx) {
        ExprNode::postprocess(ctx);
        // Verify that manual use of addArg() didn't produce the wrong number of args:
        size_t nArgs = std::min(_operands.size(), size_t(9));
        Assert(nArgs >= _op.minArgs && nArgs <= _op.maxArgs);
    }
#endif


#pragma mark - ANY/EVERY NODE:

    AnyEveryNode::AnyEveryNode(Operation const& op, Array::iterator& operands, ParseContext& ctx)
        : OpNode(op, operands, ctx) {
        if ( auto lit = dynamic_cast<LiteralNode*>(_operands[0]) ) _variableName = lit->asString();
        require(isValidIdentifier(_variableName), "invalid variable name in ANY/EVERY");
    }

#pragma mark - FUNCTION NODE:

    ExprNode* FunctionNode::parse(slice name, Array::iterator& args, ParseContext& ctx) {
        auto& spec = lookupFn(name, args.count());
        auto  fn   = new (ctx) FunctionNode(spec);
        fn->addArgs(args, ctx);

        if ( spec.name == kArrayCountFnName ) {
            if ( auto prop = dynamic_cast<PropertyNode*>(fn->_args.front()) ) {
                // Special case: "array_count(propertyname)" turns into a call to fl_count:
                fn->_args.pop_front();
                prop->setParent(nullptr);
                prop->setSQLiteFn(kCountFnName);
                return prop;
            }
        }

        if ( fn->spec().flags & kOpWantsCollation ) fn->_collation = ctx.collation;

        return fn;
    }

    void FunctionNode::addArgs(fleece::Array::iterator& args, ParseContext& ctx) {
        for ( ; args; ++args ) addArg(ExprNode::parse(*args, ctx));
    }

    OpFlags FunctionNode::opFlags() const { return _fn.flags; }

    void FunctionNode::visitChildren(ChildVisitor const& visitor) { visitor(_args); }

    void FunctionNode::postprocess(ParseContext& ctx) {
        ExprNode::postprocess(ctx);
#if DEBUG
        // Verify that manual use of addArg() didn't produce the wrong number of args.
        size_t nArgs = std::min(_args.size(), size_t(9));
        Assert(nArgs >= _fn.minArgs && nArgs <= _fn.maxArgs, "wrong number of args (%zu) for %.*s", nArgs,
               FMTSLICE(_fn.name));
#endif
        if ( _collation ) {
            // Add implicit collation arg to functions that take one:
            addArg(new (ctx) LiteralNode(ctx.newString(_collation->sqliteName())));
        }
    }

}  // namespace litecore::qt
