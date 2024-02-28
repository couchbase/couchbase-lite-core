//
// ExprNodes.hh
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
#include "Base.hh"
#include "UnicodeCollator.hh"
#include "fleece/function_ref.hh"
#include "fleece/Fleece.hh"
#include "fleece/Mutable.hh"
#include <iosfwd>
#include <unordered_map>

namespace litecore::qt {
    using namespace fleece;

    struct Operation;
    class SQLWriter;
    class AliasedNode;
    class SelectNode;
    class SourceNode;
    class WhatNode;

    enum class MetaProperty {
        none,
        id,
        sequence,
        deleted,
        expiration,
        revisionID,
        rowid,

        _notDeleted = -1    // used internally
    };
    static constexpr size_t kNumMetaProperties = 6; // ignoring `none` and `_notDeleted`

    enum class JoinType { none = -1, inner = 0, left, leftOuter, cross };

    enum OpFlags {
        kOpNoFlags          = 0x00,
        kOpBoolResult       = 0x02, // Result is boolean
        kOpNumberResult     = 0x04, // Result is a number
        kOpStringResult     = 0x08, // Result is a string
        kOpAggregate        = 0x10, // Is this an aggregate function?
        kOpWantsCollation   = 0x20, // Does this function support a collation argument?
    };

    enum class IndexType {
        none,
        FTS,
        vector
    };


    /** State used during parsing. */
    struct ParseContext {
        SelectNode* select = nullptr;                   // The enclosing SELECT, if any
        std::unordered_map<string, AliasedNode*> aliases;// All of the sources & named results
        std::vector<SourceNode*> sources;               // All sources
        SourceNode* from = nullptr;                     // The main source
        Collation collation;                            // Current collation in effect
        bool collationApplied = true;                   // False if no COLLATE node generated
    };


    /** Abstract syntax tree node for parsing N1QL queries from JSON/Fleece. */
    class Node {
    public:
        Node() = default;
        virtual ~Node() = default;

        /// Writes SQLite-flavor SQL representation to a stream.
        void writeSQL(std::ostream& out) const;

        /// Returns SQLite-flavor SQL representation.
        string SQLString() const;

        using Visitor = function_ref<void(Node&, unsigned depth)>;

        /// The Visitor callback will be called with this Node and each of its descendents,
        /// in depth-first order.
        virtual void visit(Visitor const& visitor, unsigned depth = 0) {
            visitor(*this, depth + 1);
        }

        using Rewriter = function_ref<Node*(Node*)>;
        /// The Rewriter callback will be called with each of this Node's descendents,
        /// in depth-first order. If it returns a different Node (or nullptr), that value will
        /// replace the child in-place.
        virtual void rewriteChildren(const Rewriter&) { }

        template <class N, class FN>
        void visitType(FN const& callback) {
            visit([&](Node& node, unsigned) {
                if (auto n = dynamic_cast<N*>(&node))
                    callback(*n);
            });
        }

        template <class N>
        void rewriteType(function_ref<Node*(N*)> const& callback) {
            rewriteChildren([&](Node* node) {
                if (auto n = dynamic_cast<N*>(&node))
                    return callback(n);
                else
                    return node;
            });
        }

        /// Called after the Node tree is generated; allows each node to make changes,
        /// including replacing its children or even itself.
        virtual Node* postprocess(ParseContext& ctx) {
            rewriteChildren([&](Node* child) { return child->postprocess(ctx); });
            return this;
        }

        virtual void writeSQL(SQLWriter&) const = 0;

    protected:
        /// Overrides of `rewriteChildren` should call this on each child Node.
        /// It calls the Rewriter; if the child is not replaced it then calls rewriteChildren on it.
        /// @returns  true if the child was replaced.
        template <class N>
        bool rewriteChild(unique_ptr<N>& child, const Rewriter& r) {
            if (child) {
                Node* replacement = r(child.get());
                if (replacement != child.get()) {
                    auto typeSafeReplacement = dynamic_cast<N*>(replacement);
                    if (!typeSafeReplacement && replacement) throw std::bad_cast();
                    child = unique_ptr<N>(typeSafeReplacement);
                    return true;
                } else {
//                    child->rewriteChildren(r);
                }
            }
            return false;
        }

        Node(Node const&) = delete;
        Node& operator=(Node const&) = delete;
    };


#pragma mark - EXPRESSIONS:


    /** An expression. (abstract) */
    class ExprNode : public Node {
    public:
        /// Parses an expression from a Fleece Value.
        static unique_ptr<ExprNode> parse(Value, ParseContext&);

        /// The column name to use if this expression is the child of a WhatNode.
        virtual string asColumnName() const         {return "";}

        virtual OpFlags opFlags() const             {return kOpNoFlags;}

    private:
        static unique_ptr<ExprNode> parseArray(Array, ParseContext&);
        static unique_ptr<ExprNode> parseOp(Operation const&, Array::iterator, ParseContext&);
    };

    /** A literal value. */
    class LiteralNode final : public ExprNode {
    public:
        explicit LiteralNode(Value);
        explicit LiteralNode(string_view);
        Value literal() const                       {return _literal;}
        void writeSQL(SQLWriter&) const override;

    private:
        Value           _literal;
        RetainedValue   _retained;
    };

    /** The magic `meta()` or `meta('collection')` function. */
    class MetaNode final : public ExprNode {
    public:
        explicit MetaNode(Array::iterator& args, ParseContext&);
        explicit MetaNode(MetaProperty p, SourceNode* src)      :_property(p), _source(src) { }
        MetaProperty property() const                           {return _property;}
        SourceNode* source() const                              {return _source;}
        string asColumnName() const override;
        OpFlags opFlags() const override;

        void writeSQL(SQLWriter&) const override;
        static void writeMetaSQL(string_view aliasDot, MetaProperty, SQLWriter&);

    private:
        MetaProperty _property = MetaProperty::none;
        SourceNode* _source = nullptr;
    };

    /** A query parameter ("$foo") in an expression. */
    class ParameterNode final : public ExprNode {
    public:
        explicit ParameterNode(slice name);
        explicit ParameterNode(Value v)                         :ParameterNode(v.toString()) { }

        string_view name() const                                {return _name;}

        void writeSQL(SQLWriter&) const override;

    private:
        string _name;       // Parameter name (without the '$')
    };

    /** A document property in an expression. */
    class PropertyNode final : public ExprNode {
    public:
        static unique_ptr<ExprNode> parse(slice pathStr, Array::iterator* operands, ParseContext&);

        SourceNode* source() const                              {return _source;}
        KeyPath const& path() const                             {return _path;}
        string asColumnName() const override;
        void setSQLiteFn(string_view fn)                        {_sqliteFn = string(fn);}

        void writeSQL(SQLWriter& ctx) const override            {writeSQL(ctx, nullslice, nullptr);}
        void writeSQL(SQLWriter&, slice sqliteFnName, ExprNode* param) const;

    private:
        PropertyNode(SourceNode* src, WhatNode* result, KeyPath path, string fn)
        :_source(src), _result(result), _path(std::move(path)), _sqliteFn(std::move(fn)) { }

        SourceNode*     _source;        // Source I am relative to
        WhatNode*       _result;        // Result I am relative to (only if _source is nullptr)
        KeyPath         _path;          // The path (possibly empty)
        string          _sqliteFn;      // SQLite function to emit; usually `fl_value`
    };

    /** A local variable used in an ANY/EVERY expression. */
    class VariableNode final : public ExprNode {
    public:
        static unique_ptr<ExprNode> parse(slice op, Array::iterator& args, ParseContext&);
        explicit VariableNode(slice name);

        void writeSQL(SQLWriter&) const override;

    private:
        string  _name;                      // Variable name (without the '?')
        bool    _returnBody = false;        // If true, expands to `.body` not `.value`
    };

    /** A COLLATE clause; affects the SQLite text collation of its child node. */
    class CollateNode final : public ExprNode {
    public:
        static unique_ptr<ExprNode> parse(Dict options, Value child, ParseContext&);

        CollateNode(unique_ptr<ExprNode>, ParseContext&);
        ExprNode* child() const                                 {return _child.get();}
        Collation const& collation() const                      {return _collation;}
        bool isBinary() const;

        OpFlags opFlags() const override                        {return _child->opFlags();}
        void visit(Visitor const& visitor, unsigned depth = 0) override;
        void rewriteChildren(const Rewriter&) override;
        void writeSQL(SQLWriter&) const override;

    private:
        unique_ptr<ExprNode>    _child;         // The expression COLLATE is applied to
        Collation               _collation;     // The collation
    };

    /** A Node that just writes arbitrary SQL. Use sparingly and with caution. */
    class RawSQLNode final : public ExprNode {
    public:
        explicit RawSQLNode(string sql) :_sql(std::move(sql)) { }

        void writeSQL(SQLWriter&) const override;

    private:
        string _sql;
    };


#pragma mark - COMPOUND EXPRESSIONS:


    /** An operation in an expression. */
    class OpNode : public ExprNode {
    public:
        explicit OpNode(Operation const& op)        :_op(op) { }
        explicit OpNode(Operation const&, Array::iterator& operands, ParseContext&);

        Operation const& op() const                 {return _op;}
        ExprNode* operand(size_t i) const           {return _operands.at(i).get();}

        void addArg(unique_ptr<ExprNode> node)      {_operands.emplace_back(std::move(node));}

        OpFlags opFlags() const override;
        void visit(Visitor const& visitor, unsigned depth = 0) override;
        void rewriteChildren(const Rewriter&) override;
        void writeSQL(SQLWriter&) const override;

        Node* postprocess(ParseContext&) override;

    protected:
        Operation const&                    _op;
        std::vector<unique_ptr<ExprNode>>   _operands;
    };

    /** A N1QL function call in an expression. */
    class FunctionNode final : public ExprNode {
    public:
        static unique_ptr<ExprNode> parse(slice name, Array::iterator &args, ParseContext&);

        explicit FunctionNode(struct FunctionSpec const& fn)    :_fn(fn) { }

        void addArg(unique_ptr<ExprNode> n)                     {_args.emplace_back(std::move(n));}
        void addArgs(Array::iterator&, ParseContext&);

        std::vector<unique_ptr<ExprNode>> const& args() const   {return _args;}
        struct FunctionSpec const& spec()                       {return _fn;}

        OpFlags opFlags() const override;
        void visit(Visitor const& visitor, unsigned depth = 0) override;
        void rewriteChildren(const Rewriter&) override;
        void writeSQL(SQLWriter&) const override;

    private:
        struct FunctionSpec const&          _fn;
        std::vector<unique_ptr<ExprNode>>   _args;
    };

    /** An OpNode representing an `ANY`, `EVERY`, or `ANY AND EVERY` operation */
    class AnyEveryNode final : public OpNode {
    public:
        explicit AnyEveryNode(Operation const&, Array::iterator& operands, ParseContext&);

        void writeSQL(SQLWriter&) const override;

        string_view variableName() const  {return _variableName;}
        ExprNode& collection() const      {return *_operands[1];}
        ExprNode& predicate() const       {return *_operands[2];}

        OpFlags opFlags() const override            {return kOpBoolResult;}

    private:
        slice _variableName;                        // Name of the variable used in predicate
    };
}
