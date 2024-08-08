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
#include "Error.hh"
#include "UnicodeCollator.hh"
#include "checked_ptr.hh"
#include "fleece/function_ref.hh"
#include "fleece/Fleece.hh"
#include "fleece/Mutable.hh"
#include <iosfwd>
#include <optional>
#include <unordered_map>

C4_ASSUME_NONNULL_BEGIN

namespace litecore::qt {
    using namespace fleece;

    struct Operation;
    class SQLWriter;
    class AliasedNode;
    class SelectNode;
    class SourceNode;
    class WhatNode;

    /** Properties of the N1QL `meta()` object. */
    enum class MetaProperty {
        none,
        id,
        sequence,
        deleted,
        expiration,
        revisionID,
        rowid,

        _notDeleted = -1  // used internally
    };
    static constexpr size_t kNumMetaProperties = 6;  // ignoring `none` and `_notDeleted`

    /** Types of JOINs. */
    enum class JoinType { none = -1, inner = 0, left, leftOuter, cross };

    /** Attributes of an operation. */
    enum OpFlags {
        kOpNoFlags        = 0x00,
        kOpBoolResult     = 0x02,  // Result is boolean
        kOpNumberResult   = 0x04,  // Result is a number
        kOpStringResult   = 0x08,  // Result is a string
        kOpAggregate      = 0x10,  // This is an aggregate function
        kOpWantsCollation = 0x20,  // This function supports a collation argument
    };

    /** Types of indexes. */
    enum class IndexType { none, FTS, vector };

    /** State used during parsing, passed down through the recursive descent. */
    struct ParseContext {
        checked_ptr<SelectNode>                              select;     // The enclosing SELECT, if any
        std::unordered_map<string, checked_ptr<AliasedNode>> aliases;    // All of the sources & named results
        std::vector<checked_ptr<SourceNode>>                 sources;    // All sources
        checked_ptr<SourceNode>                              from;       // The main source
        Collation                                            collation;  // Current collation in effect
        bool collationApplied = true;                                    // False if no COLLATE node generated

        void clear();
    };

    /** Abstract syntax tree node for parsing N1QL queries from JSON/Fleece.
        Nodes are heap-allocated via unique_ptr and are not copyable.
        The Node class hierarchy is described in docs/QueryTranslator.md */
    class Node : public checked_target {
      public:
        Node()          = default;
        virtual ~Node() = default;

        /// The node's parent in the parse tree.
        Node const* parent() const { return _parent; }

        void setParent(Node* C4NULLABLE);

        /// The SourceNode (`FROM` item) this references, if any. Overridden by MetaNode and PropertyNode.
        virtual SourceNode* C4NULLABLE source() const { return nullptr; }

        using VisitorFn = function_ref<void(Node&, unsigned depth)>;

        /// The Visitor callback will be called with this Node and each of its descendents.
        /// @param visitor  The callback
        /// @param preorder  If true, a Node is visited before its children; else after
        /// @param depth  The initial depth corresponding to this Node.
        void visitTree(VisitorFn const& visitor, bool preorder = true, unsigned depth = 0);

        /// Called after the Node tree is generated; allows each node to make changes.
        /// Overrides must call the inherited method, probably first.
        virtual void postprocess(ParseContext& ctx) {
            visitChildren(ChildVisitor{[&](Node& child) { child.postprocess(ctx); }});
        }

        /// Returns SQLite-flavor SQL representation.
        string SQLString() const;

        /// Writes SQLite-flavor SQL representation to a stream.
        void writeSQL(std::ostream& out) const;

        /// Writes SQL to the writer's output stream.
        virtual void writeSQL(SQLWriter&) const = 0;

      protected:
        /// Utility to set a child reference, ensuring its parent points to me.
        template <class T>
        void setChild(unique_ptr<T>& var, unique_ptr<T> c) {
            if ( var ) var->setParent(nullptr);
            if ( c ) c->setParent(this);
            var = std::move(c);
        }

        /// Utility to add a child reference, ensuring its parent points to me.
        template <class T>
        void addChild(std::vector<unique_ptr<T>>& var, unique_ptr<T> c) {
            c->setParent(this);
            var.emplace_back(std::move(c));
        }

        struct ChildVisitor {
            function_ref<void(Node&)> fn;

            template <class T>
            ChildVisitor const& operator()(unique_ptr<T> const& child) const {
                if ( child ) fn(*child);
                return *this;
            }

            template <class T>
            ChildVisitor const& operator()(std::vector<T> const& children) const {
                for ( auto& child : children ) fn(*child);
                return *this;
            }
        };

        /// Subclasses that add children MUST override this and call `visitor(child)` on each direct child.
        virtual void visitChildren(ChildVisitor const& visitor) {}

        Node(Node const&)            = delete;  // not copyable
        Node& operator=(Node const&) = delete;

        checked_ptr<const Node> _parent;
    };

#pragma mark - EXPRESSIONS:

    /** An expression. (abstract) */
    class ExprNode : public Node {
      public:
        /// Parses an expression from a Fleece Value.
        static unique_ptr<ExprNode> parse(Value, ParseContext&);

        /// The column name to use if this expression is the child of a WhatNode.
        virtual string asColumnName() const { return ""; }

        /// The operation flags that apply to this expression.
        virtual OpFlags opFlags() const { return kOpNoFlags; }

      private:
        static unique_ptr<ExprNode> parseArray(Array, ParseContext&);
        static unique_ptr<ExprNode> parseOp(Operation const&, Array::iterator, ParseContext&);
        static unique_ptr<ExprNode> parseInNotIn(Operation const&, Array::iterator&, ParseContext&);
        static unique_ptr<ExprNode> parseExists(Operation const&, Array::iterator&, ParseContext&);
        static unique_ptr<ExprNode> parseBlob(Operation const&, Array::iterator&, ParseContext&);
        static unique_ptr<ExprNode> parseObjectProperty(Operation const&, Array::iterator&, ParseContext&);
    };

    /** A literal value. */
    class LiteralNode final : public ExprNode {
      public:
        explicit LiteralNode(Value);
        explicit LiteralNode(int64_t);
        explicit LiteralNode(string_view);

        Value literal() const { return _literal; }

        void writeSQL(SQLWriter&) const override;

      private:
        Value         _literal;
        RetainedValue _retained;
    };

    /** The magic `meta()` or `meta('collection')` function, or one of its properties. */
    class MetaNode final : public ExprNode {
      public:
        explicit MetaNode(Array::iterator& args, ParseContext&);

        explicit MetaNode(MetaProperty p, SourceNode* C4NULLABLE src);

        MetaProperty property() const { return _property; }

        SourceNode* source() const override;
        string      asColumnName() const override;
        OpFlags     opFlags() const override;
        void        writeSQL(SQLWriter&) const override;
        static void writeMetaSQL(string_view aliasDot, MetaProperty, SQLWriter&);

      private:
        MetaProperty            _property = MetaProperty::none;
        checked_ptr<SourceNode> _source;
    };

    /** A query parameter (`$foo`) in an expression. */
    class ParameterNode final : public ExprNode {
      public:
        explicit ParameterNode(slice name);

        explicit ParameterNode(Value v) : ParameterNode(v.toString()) {}

        string_view name() const { return _name; }

        void writeSQL(SQLWriter&) const override;

      private:
        string _name;  // Parameter name (without the '$')
    };

    /** A document property path in an expression. */
    class PropertyNode final : public ExprNode {
      public:
        /// Parses a JSON property expression like `[".foo.bar"]` or `[".", "foo", "bar"]`.
        /// @param pathStr  The initial path string; may be empty.
        /// @param operands  Array of path components (strings or ints), or nullptr.
        /// @param ctx  Parse context.
        static unique_ptr<ExprNode> parse(slice pathStr, Array::iterator* C4NULLABLE operands, ParseContext& ctx);

        /// The parsed path as a Fleece KeyPath.
        KeyPath const& path() const { return _path; }

        /// Sets the SQLite function used to dereference the property; default is `fl_value`
        void setSQLiteFn(string_view fn) { _sqliteFn = string(fn); }

        SourceNode* source() const override;
        string      asColumnName() const override;

        void writeSQL(SQLWriter& ctx) const override { writeSQL(ctx, nullslice, nullptr); }

        void writeSQL(SQLWriter&, slice sqliteFnName, ExprNode* C4NULLABLE param) const;

      private:
        PropertyNode(SourceNode* C4NULLABLE src, WhatNode* C4NULLABLE result, KeyPath path, string fn);

        checked_ptr<SourceNode> _source;    // Source I am relative to
        checked_ptr<WhatNode>   _result;    // Result I am relative to (only if _source is nullptr)
        KeyPath                 _path;      // The path (possibly empty)
        string                  _sqliteFn;  // SQLite function to emit; usually `fl_value`
    };

    /** A local variable (`?foo`) used in an ANY/EVERY expression. */
    class VariableNode final : public ExprNode {
      public:
        static unique_ptr<ExprNode> parse(slice op, Array::iterator& args, ParseContext&);
        explicit VariableNode(slice name);

        void writeSQL(SQLWriter&) const override;

      private:
        string _name;                // Variable name (without the '?')
        bool   _returnBody = false;  // If true, expands to `.body` not `.value`
    };

    /** A COLLATE clause; affects the SQLite text collation of its child node. */
    class CollateNode final : public ExprNode {
      public:
        static unique_ptr<ExprNode> parse(Dict options, Value child, ParseContext&);

        CollateNode(unique_ptr<ExprNode>, ParseContext&);

        ExprNode* child() const { return _child.get(); }  ///< The expression to which the collation is applied

        Collation const& collation() const { return _collation; }  ///< The collation used

        bool isBinary() const;  ///< True if collation is binary (i.e. neither case-sensitive nor Unicode)

        OpFlags opFlags() const override { return _child->opFlags(); }

        void visitChildren(ChildVisitor const& visitor) override;
        void writeSQL(SQLWriter&) const override;

      private:
        unique_ptr<ExprNode> _child;      // The expression COLLATE is applied to
        Collation            _collation;  // The collation
    };

    /** A Node that just writes arbitrary SQL. Use sparingly and with caution. */
    class RawSQLNode final : public ExprNode {
      public:
        explicit RawSQLNode(string sql) : _sql(std::move(sql)) {}

        void writeSQL(SQLWriter&) const override;

      private:
        string _sql;
    };

#pragma mark - COMPOUND EXPRESSIONS:

    /** An operation in an expression. (Not abstract, but has a subclass `AnyEveryNode`.) */
    class OpNode : public ExprNode {
      public:
        explicit OpNode(Operation const& op) : _op(op) {}

        explicit OpNode(Operation const&, Array::iterator& operands, ParseContext&);

        Operation const& op() const { return _op; }

        ExprNode* operand(size_t i) const { return _operands.at(i).get(); }

        void addArg(unique_ptr<ExprNode> node) { addChild(_operands, std::move(node)); }

        OpFlags opFlags() const override;
        void    visitChildren(ChildVisitor const& visitor) override;
        void    writeSQL(SQLWriter&) const override;
#if DEBUG
        void postprocess(ParseContext&) override;
#endif

      protected:
        Operation const&                  _op;        // Spec of the operation
        std::vector<unique_ptr<ExprNode>> _operands;  // Operand list
    };

    /** A N1QL function call in an expression. */
    class FunctionNode final : public ExprNode {
      public:
        static unique_ptr<ExprNode> parse(slice name, Array::iterator& args, ParseContext&);

        explicit FunctionNode(struct FunctionSpec const& fn) : _fn(fn) {}

        void addArg(unique_ptr<ExprNode> n) { addChild(_args, std::move(n)); }

        void addArgs(Array::iterator&, ParseContext&);

        std::vector<unique_ptr<ExprNode>> const& args() const { return _args; }

        struct FunctionSpec const& spec() { return _fn; }

        OpFlags opFlags() const override;
        void    visitChildren(ChildVisitor const& visitor) override;
        void    writeSQL(SQLWriter&) const override;
        void postprocess(ParseContext&) override;

      private:
        struct FunctionSpec const&        _fn;         // Spec of the function
        std::vector<unique_ptr<ExprNode>> _args;       // Argument list
        std::optional<Collation>          _collation;  // Collation arg to add last
    };

    /** An OpNode representing an `ANY`, `EVERY`, or `ANY AND EVERY` operation */
    class AnyEveryNode final : public OpNode {
      public:
        explicit AnyEveryNode(Operation const&, Array::iterator& operands, ParseContext&);

        string_view variableName() const { return _variableName; }

        ExprNode& collection() const { return *_operands[1]; }

        ExprNode& predicate() const { return *_operands[2]; }

        OpFlags opFlags() const override { return kOpBoolResult; }

        void writeSQL(SQLWriter&) const override;

      private:
        slice _variableName;  // Name of the variable used in predicate
    };

}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
