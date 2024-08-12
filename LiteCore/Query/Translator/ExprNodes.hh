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
#include "Node.hh"
#include <optional>

C4_ASSUME_NONNULL_BEGIN

namespace litecore::qt {
    using namespace fleece;

    struct Operation;
    class WhatNode;

    /** An expression. (abstract) */
    class ExprNode : public Node {
      public:
        /// Parses an expression from a Fleece Value.
        static ExprNode* parse(Value, ParseContext&) C4_RETURNS_NONNULL;

        /// The column name to use if this expression is the child of a WhatNode.
        virtual string asColumnName() const { return ""; }

        /// The operation flags that apply to this expression.
        virtual OpFlags opFlags() const { return kOpNoFlags; }

      private:
        static ExprNode* parseArray(Array, ParseContext&) C4_RETURNS_NONNULL;
        static ExprNode* parseOp(Operation const&, Array::iterator, ParseContext&) C4_RETURNS_NONNULL;
        static ExprNode* parseInNotIn(Operation const&, Array::iterator&, ParseContext&) C4_RETURNS_NONNULL;
        static ExprNode* parseExists(Operation const&, Array::iterator&, ParseContext&) C4_RETURNS_NONNULL;
        static ExprNode* parseBlob(Operation const&, Array::iterator&, ParseContext&) C4_RETURNS_NONNULL;
        static ExprNode* parseObjectProperty(Operation const&, Array::iterator&, ParseContext&) C4_RETURNS_NONNULL;
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
        MetaProperty           _property = MetaProperty::none;  // The property of `meta()` being accessed
        SourceNode* C4NULLABLE _source{};                       // The collection
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
        static ExprNode* parse(slice pathStr, Array::iterator* C4NULLABLE operands, ParseContext& ctx);

        /// The parsed path as a Fleece KeyPath.
        KeyPath const& path() const { return _path; }

        /// Sets the SQLite function used to dereference the property; default is `fl_value`
        void setSQLiteFn(string_view fn) { _sqliteFn = fn; }

        SourceNode* source() const override;
        string      asColumnName() const override;

        void writeSQL(SQLWriter& ctx) const override { writeSQL(ctx, nullslice, nullptr); }

        void writeSQL(SQLWriter&, slice sqliteFnName, ExprNode* C4NULLABLE param) const;

      private:
        PropertyNode(SourceNode* C4NULLABLE src, WhatNode* C4NULLABLE result, KeyPath path, string_view fn);

        SourceNode* C4NULLABLE _source{};  // Source I am relative to
        WhatNode* C4NULLABLE   _result{};  // Result I am relative to (only if _source is nullptr)
        KeyPath                _path;      // The path (possibly empty)
        string_view            _sqliteFn;  // SQLite function to emit; usually `fl_value`
    };

    /** A local variable (`?foo`) used in an ANY/EVERY expression. */
    class VariableNode final : public ExprNode {
      public:
        static ExprNode* parse(slice op, Array::iterator& args, ParseContext&);
        explicit VariableNode(slice name);

        void writeSQL(SQLWriter&) const override;

      private:
        string _name;                // Variable name (without the '?')
        bool   _returnBody = false;  // If true, expands to `.body` not `.value`
    };

    /** A COLLATE clause; affects the SQLite text collation of its child node. */
    class CollateNode final : public ExprNode {
      public:
        static ExprNode* parse(Dict options, Value child, ParseContext&);

        CollateNode(ExprNode*, ParseContext&);

        ExprNode* child() const { return _child; }  ///< The expression to which the collation is applied

        Collation const& collation() const { return _collation; }  ///< The collation used

        bool isBinary() const;  ///< True if collation is binary (i.e. neither case-sensitive nor Unicode)

        OpFlags opFlags() const override { return _child->opFlags(); }

        void visitChildren(ChildVisitor const& visitor) override;
        void writeSQL(SQLWriter&) const override;

      private:
        ExprNode* _child;      // The expression COLLATE is applied to
        Collation _collation;  // The collation
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

        ExprNode* operand(size_t i) const { return _operands.at(i); }

        void addArg(ExprNode* node) { addChild(_operands, node); }

        OpFlags opFlags() const override;
        void    visitChildren(ChildVisitor const& visitor) override;
        void    writeSQL(SQLWriter&) const override;
#if DEBUG
        void postprocess(ParseContext&) override;
#endif

      protected:
        Operation const&       _op;        // Spec of the operation
        std::vector<ExprNode*> _operands;  // Operand list
    };

    /** A N1QL function call in an expression. */
    class FunctionNode final : public ExprNode {
      public:
        static ExprNode* parse(slice name, Array::iterator& args, ParseContext&);

        explicit FunctionNode(struct FunctionSpec const& fn) : _fn(fn) {}

        void addArg(ExprNode* n) { addChild(_args, std::move(n)); }

        void addArgs(Array::iterator&, ParseContext&);

        std::vector<ExprNode*> const& args() const { return _args; }

        struct FunctionSpec const& spec() { return _fn; }

        OpFlags opFlags() const override;
        void    visitChildren(ChildVisitor const& visitor) override;
        void    writeSQL(SQLWriter&) const override;
        void    postprocess(ParseContext&) override;

      private:
        struct FunctionSpec const& _fn;         // Spec of the function
        std::vector<ExprNode*>     _args;       // Argument list
        std::optional<Collation>   _collation;  // Collation arg to add last
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
