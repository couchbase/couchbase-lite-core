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

    void Node::setParent(Node const* p) {
        DebugAssert(!_parent || !p);
        _parent = p;
    }

    void Node::visit(Visitor const& visitor, bool preorder, unsigned depth) {
        if ( preorder ) visitor(*this, depth);
        visitChildren([&](Node& child) { child.visit(visitor, preorder, depth + 1); });
        if ( !preorder ) visitor(*this, depth);
    }

#pragma mark - EXPRESSION PARSING:

    // Top-level method to parse an expression.
    unique_ptr<ExprNode> ExprNode::parse(Value v, ParseContext& ctx) {
        switch ( v.type() ) {
            case kFLUndefined:
                fail("internal error: missing Value for expression");
            case kFLNull:
            case kFLBoolean:
            case kFLNumber:
            case kFLString:
                return make_unique<LiteralNode>(v);
            case kFLData:
                fail("Binary data not supported in query");
            case kFLArray:
                return parseArray(v.asArray(), ctx);
            case kFLDict:
                {
                    // Construct a `dict_of(...)` call:
                    auto result = make_unique<FunctionNode>(kDictOfFunctionSpec);
                    for ( Dict::iterator i(v.asDict()); i; ++i ) {
                        result->addArg(make_unique<LiteralNode>(i.key()));
                        result->addArg(ExprNode::parse(i.value(), ctx));
                    }
                    return result;
                }
        }
    }

    unique_ptr<ExprNode> ExprNode::parseArray(Array array, ParseContext& ctx) {
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
            auto fn = make_unique<FunctionNode>(kNestedValueFunctionSpec);
            fn->addArg(ExprNode::parse(operands[0], ctx));
            fn->addArg(make_unique<LiteralNode>(opName.from(2)));
            return fn;
        } else if ( opName.hasPrefix('$') ) {
            require(operands.count() == 0, "extra operands to '%.*s'", FMTSLICE(opName));
            return make_unique<ParameterNode>(opName.from(1));
        } else if ( opName.hasPrefix('?') ) {
            return VariableNode::parse(opName.from(1), operands, ctx);
        } else if ( opName.hasSuffix("()"_sl) ) {
            return FunctionNode::parse(opName.upTo(opName.size - 2), operands, ctx);
        } else {
            fail("Unknown operator '%.*s'", FMTSLICE(opName));
        }
    }

    unique_ptr<ExprNode> ExprNode::parseOp(Operation const& op, Array::iterator operands, ParseContext& ctx) {
        switch ( op.type ) {
            case OpType::any:
            case OpType::every:
            case OpType::anyAndEvery:
                return make_unique<AnyEveryNode>(op, operands, ctx);
            case OpType::arrayLiteral:
                {
                    auto fn = make_unique<FunctionNode>(lookupFn(kArrayOfFnName, operands.count()));
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
                return make_unique<MatchNode>(operands, ctx);
            case OpType::meta:
                return make_unique<MetaNode>(operands, ctx);
            case OpType::objectProperty:
                return parseObjectProperty(op, operands, ctx);
            case OpType::property:
                return PropertyNode::parse(nullslice, &operands, ctx);
            case OpType::parameter:
                return make_unique<ParameterNode>(operands[0]);
            case OpType::rank:
                return make_unique<RankNode>(operands, ctx);
            case OpType::select:
                return make_unique<SelectNode>(operands[0], ctx);
            case OpType::variable:
                return VariableNode::parse(nullslice, operands, ctx);
#ifdef COUCHBASE_ENTERPRISE
            case OpType::vectorDistance:
                return make_unique<VectorDistanceNode>(operands, ctx);
#endif
            default:
                // A normal OpNode
                return make_unique<OpNode>(op, operands, ctx);
        }
    }

    unique_ptr<ExprNode> ExprNode::parseInNotIn(Operation const& op, Array::iterator& operands, ParseContext& ctx) {
        auto lhs          = parse(operands[0], ctx);
        auto arrayOperand = operands[1].asArray();
        if ( arrayOperand && arrayOperand[0].asString() == "[]"_sl ) {
            // RHS is a literal array, so use SQL "IN" syntax:
            auto result = make_unique<OpNode>(op);
            result->addArg(std::move(lhs));
            for ( unsigned i = 1; i < arrayOperand.count(); ++i ) result->addArg(parse(arrayOperand[i], ctx));
            return result;
        } else {
            // Otherwise generate a call to array_contains():
            // [yes, operands are in reverse order]
            auto contains = make_unique<FunctionNode>(lookupFn("array_contains", 2));
            contains->addArg(parse(operands[1], ctx));
            contains->addArg(std::move(lhs));
            if ( op.type == OpType::not_in ) {
                auto n = make_unique<OpNode>(*lookupOp("NOT", 1));
                n->addArg(std::move(contains));
                return n;
            } else {
                return contains;
            }
        }
    }

    unique_ptr<ExprNode> ExprNode::parseExists(Operation const& op, Array::iterator& operands, ParseContext& ctx) {
        auto arg = parse(operands[0], ctx);
        if ( auto prop = dynamic_cast<PropertyNode*>(arg.get()) ) {
            // "EXISTS propertyname" turns into a call to fl_exists()
            prop->setSQLiteFn(kExistsFnName);
            return arg;
        } else {
            auto exists = make_unique<OpNode>(op);
            exists->addArg(std::move(arg));
            return exists;
        }
    }

    unique_ptr<ExprNode> ExprNode::parseBlob(Operation const& op, Array::iterator& operands, ParseContext& ctx) {
        unique_ptr<ExprNode> arg;
        if ( slice propStr = operands[0].asString() ) {
            arg = PropertyNode::parse(propStr, nullptr, ctx);
        } else {
            arg = parse(operands[0], ctx);
            require(dynamic_cast<PropertyNode*>(arg.get()), "argument of BLOB() must be a document property");
        }
        auto blob = make_unique<OpNode>(op);
        blob->addArg(std::move(arg));
        return blob;
    }

    unique_ptr<ExprNode> ExprNode::parseObjectProperty(Operation const& op, Array::iterator& operands,
                                                       ParseContext& ctx) {
        auto node = make_unique<OpNode>(op, operands, ctx);
        // Convert ['_.', ['META()'], 'x']  ->  ['._x'] :
        auto meta = dynamic_cast<MetaNode*>(node->operand(0));
        if ( meta && meta->property() == MetaProperty::none ) {
            if ( auto key = dynamic_cast<LiteralNode*>(node->operand(1)) ) {
                if ( slice keyStr = key->literal().asString() ) {
                    // OK, matched the pattern. Now identify the key:
                    if ( keyStr.hasPrefix('.') ) keyStr.moveStart(1);
                    MetaProperty prop = lookupMeta(keyStr, kMetaPropertyNames);
                    require(prop != MetaProperty::none, "'%.*s' is not a valid Meta key", FMTSLICE(keyStr));
                    return make_unique<MetaNode>(prop, meta->source());
                }
            }
        }
        return node;
    }

#pragma mark - LEAF NODES:

    LiteralNode::LiteralNode(Value v) : _literal(v) {
        Assert(v.type() < kFLData);
        if ( v.isMutable() ) _retained = v;
    }

    LiteralNode::LiteralNode(int64_t i) : LiteralNode(RetainedValue::newInt(i)) {}

    LiteralNode::LiteralNode(string_view str) : LiteralNode(RetainedValue::newString(str)) {}

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

    string MetaNode::asColumnName() const {
        if ( _property != MetaProperty::none ) {
            return string(kMetaPropertyNames[int(_property) - 1]);
        } else if ( _source ) {
            return _source->asColumnName();
        } else {
            return "";
        }
    }

    OpFlags MetaNode::opFlags() const { return kMetaFlags[int(_property) + 1]; }

    ParameterNode::ParameterNode(slice name) : _name(name) {
        require(isAlphanumericOrUnderscore(_name), "Invalid query parameter name '%s'", _name.c_str());
    }

    unique_ptr<ExprNode> PropertyNode::parse(slice pathStr, fleece::Array::iterator* components, ParseContext& ctx) {
        KeyPath     path   = parsePath(pathStr, components);
        SourceNode* source = nullptr;
        WhatNode*   result = nullptr;
        string      sqliteFn;

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
                return make_unique<MetaNode>(meta, source);
            }
            if ( source && source->isUnnest() ) sqliteFn = kNestedValueFnName;
            else
                sqliteFn = kValueFnName;
        }
        return unique_ptr<ExprNode>(new PropertyNode(source, result, std::move(path), sqliteFn));
    }

    string PropertyNode::asColumnName() const {
        if ( _path.count() > 0 ) {
            return string(_path.get(_path.count() - 1).first);  // last path component
        } else if ( _source ) {
            return _source->asColumnName();
        } else {
            return "";
        }
    }

    unique_ptr<ExprNode> VariableNode::parse(slice pathStr, Array::iterator& args, ParseContext& ctx) {
        if ( !pathStr ) {
            pathStr = requiredString(*args, "variable name");
            ++args;
        }
        KeyPath path = parsePath(pathStr, &args);
        require(path.count() > 0, "invalid variable name");
        slice varName = path.get(0).first;
        auto  var     = make_unique<VariableNode>(varName);

        if ( path.count() == 1 ) {
            return var;
        } else {
            // There's a path after the variable name. Expand this to a property access:
            path.dropComponents(1);
            var->_returnBody = true;

            auto expr = make_unique<OpNode>(lookupOp(OpType::objectProperty));
            expr->addArg(std::move(var));
            expr->addArg(make_unique<LiteralNode>(path.toString()));
            return expr;
        }
    }

    VariableNode::VariableNode(slice name) : _name(name) {
        require(isValidIdentifier(_name), "Invalid variable name '%s'", _name.c_str());
    }

#pragma mark - COLLATE NODE:

    unique_ptr<ExprNode> CollateNode::parse(Dict options, Value childVal, ParseContext& ctx) {
        // a COLLATE op merely changes the current collation.
        // First update the current Collation from the options dict and push it:
        Collation savedCollation        = ctx.collation;
        bool      savedCollationApplied = ctx.collationApplied;

        auto setFlagFromOption = [](bool& flag, Dict options, slice key) {
            if ( Value val = getCaseInsensitive(options, key) ) flag = val.asBool();
        };
        setFlagFromOption(ctx.collation.caseSensitive, options, "CASE");
        setFlagFromOption(ctx.collation.diacriticSensitive, options, "DIAC");
        setFlagFromOption(ctx.collation.unicodeAware, options, "UNICODE");
        if ( Value localeName = getCaseInsensitive(options, "LOCALE") )
            ctx.collation.localeName = localeName.asString();
        ctx.collationApplied = false;

        // Parse the child:
        auto node = ExprNode::parse(childVal, ctx);

        if ( !ctx.collationApplied ) {
            // If no nested node used the collation, insert it into the tree here
            // so it will get written:
            node = make_unique<CollateNode>(std::move(node), ctx);
        }

        // Finally pop the saved Collation:
        ctx.collation        = savedCollation;
        ctx.collationApplied = savedCollationApplied;
        return node;
    }

    CollateNode::CollateNode(unique_ptr<ExprNode> child, ParseContext& ctx)
        : _collation(ctx.collation), _child(std::move(child)) {
        _child->setParent(this);
        ctx.collationApplied = true;
    }

    bool CollateNode::isBinary() const { return _collation.caseSensitive && !_collation.unicodeAware; }

    void CollateNode::visitChildren(ChildVisitor const& visitor) { visitor(*_child); }

#pragma mark - OP NODE:

    OpNode::OpNode(Operation const& op, Array::iterator& operands, ParseContext& ctx) : OpNode(op) {
        for ( ; operands; ++operands ) addArg(ExprNode::parse(*operands, ctx));

        if ( !ctx.collationApplied && (op.type == OpType::infix || op.type == OpType::like) ) {
            _operands[0]->setParent(nullptr);
            _operands[0] = make_unique<CollateNode>(std::move(_operands[0]), ctx);
            _operands[0]->setParent(this);
        }
    }

    OpFlags OpNode::opFlags() const { return _op.flags; }

    void OpNode::visitChildren(ChildVisitor const& visitor) {
        for ( auto& operand : _operands ) visitor(*operand);
    }

#if DEBUG
    void OpNode::postprocess(ParseContext& ctx) {
        ExprNode::postprocess(ctx);
        // Verify that manual use of addArg() didn't produce the wrong number of args:
        size_t nArgs = std::min(_operands.size(), size_t(9));
        Assert(nArgs >= _op.minArgs && nArgs <= _op.maxArgs);
    }
#endif


    AnyEveryNode::AnyEveryNode(Operation const& op, Array::iterator& operands, ParseContext& ctx)
        : OpNode(op, operands, ctx) {
        if ( auto lit = dynamic_cast<LiteralNode*>(_operands[0].get()) ) _variableName = lit->literal().asString();
        require(isValidIdentifier(_variableName), "invalid variable name in ANY/EVERY");
    }

#pragma mark - FUNCTION NODE:

    unique_ptr<ExprNode> FunctionNode::parse(slice name, Array::iterator& args, ParseContext& ctx) {
        auto& spec = lookupFn(name, args.count());
        auto  fn   = make_unique<FunctionNode>(spec);
        fn->addArgs(args, ctx);

        if ( spec.name == kArrayCountFnName ) {
            if ( auto prop = dynamic_cast<PropertyNode*>(fn->_args[0].get()) ) {
                // Special case: "array_count(propertyname)" turns into a call to fl_count:
                prop->setSQLiteFn(kCountFnName);
                prop->setParent(nullptr);
                return std::move(fn->_args[0]);
            }
        }

        if ( fn->spec().flags & kOpWantsCollation ) {
            fn->addArg(make_unique<LiteralNode>(ctx.collation.sqliteName()));
        }

        return fn;
    }

    void FunctionNode::addArgs(fleece::Array::iterator& args, ParseContext& ctx) {
        for ( ; args; ++args ) addArg(ExprNode::parse(*args, ctx));
    }

    OpFlags FunctionNode::opFlags() const { return _fn.flags; }

    void FunctionNode::visitChildren(ChildVisitor const& visitor) {
        for ( auto& arg : _args ) visitor(*arg);
    }


#if DEBUG
    void FunctionNode::postprocess(ParseContext& ctx) {
        ExprNode::postprocess(ctx);
        // Verify that manual use of addArg() didn't produce the wrong number of args.
        // (The kOpWantsCollation flag indicates that an extra collation arg can be added.)
        size_t nArgs = std::min(_args.size(), size_t(9));
        Assert(nArgs >= _fn.minArgs && nArgs <= _fn.maxArgs + ((_fn.flags & kOpWantsCollation) != 0),
               "wrong number of args (%zu) for %.*s", nArgs, FMTSLICE(_fn.name));
    }
#endif

}  // namespace litecore::qt
