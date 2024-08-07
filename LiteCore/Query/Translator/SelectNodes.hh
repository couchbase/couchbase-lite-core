//
// SelectNodes.hh
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
#include "ExprNodes.hh"
#include <unordered_set>

C4_ASSUME_NONNULL_BEGIN

namespace fleece {
    class delimiter;
}

namespace litecore::qt {
    class IndexedNode;

    /** A Node that can be named with `AS`. The abstract base class of `WhatNode` and `SourceNode`. */
    class AliasedNode : public Node {
      public:
        /// The column alias.
        string const& alias() const { return _alias; }

        /// True if an alias was set explicitly by an `AS` expression.
        bool hasExplicitAlias() const { return _hasExplicitAlias; }

        /// If the path refers to this node (first component matches its alias), removes the first component and
        /// returns true. Else returns false.
        virtual bool matchPath(KeyPath& path) const;

      protected:
        string _alias;                     // Name I'm referred to by
        bool   _hasExplicitAlias = false;  // _alias was given by an AS property
    };

    /** A projection returned by a query; an item in the `WHAT` clause. Wraps an ExprNode. */
    class WhatNode final : public AliasedNode {
      public:
        WhatNode(Value, ParseContext&);

        explicit WhatNode(unique_ptr<ExprNode> expr) : _expr(std::move(expr)) { _expr->setParent(this); }

        /// The name of the result column. If not explicitly set, makes one up based on the expression.
        string columnName() const;

        /// Explicitly sets the column name.
        void setColumnName(string s) { _columnName = std::move(s); }

        void visitChildren(ChildVisitor const&) override;
        void writeSQL(SQLWriter&) const override;
        bool matchPath(KeyPath& path) const override;

      private:
        friend class SelectNode;
        void parseChildExprs(ParseContext&);
        void ensureUniqueColumnName(std::unordered_set<string>& columnNames);

        unique_ptr<ExprNode> _expr;                 // The value to return
        Value                _tempChild;            // Temporarily holds source of _expr
        string               _columnName;           // Computed name of the result column
        bool                 _parsingExpr = false;  // kludgy temporary flag
    };

    /** An item in the `FROM` clause: a collection, join, unnested expression, or table-based index.
        Table-based indexes don't appear in the N1QL query; their nodes are added during parsing in response to
        functions such as `MATCH()` and `APPROX_VECTOR_DISTANCE()`. */
    class SourceNode final : public AliasedNode {
      public:
        explicit SourceNode(Dict, ParseContext&);
        explicit SourceNode(string_view alias);
        explicit SourceNode(IndexedNode&);

        string const& scope() const { return _scope; }  ///< Scope name, or empty if default

        string const& collection() const { return _collection; }  ///< Collection name, or empty if default

        bool usesDeletedDocs() const { return _usesDeleted; }  ///< True if exprs refer to deleted docs

        string_view tableName() const { return _tableName; }  ///< SQLite table name (set by QueryTranslator)

        void setTableName(string_view name) { _tableName = name; }  ///< Sets SQLite table name

        string asColumnName() const { return _columnName; }  ///< Name to use, if used as result column

        JoinType joinType() const { return _join; }  ///< The type of join, else `none`

        bool isJoin() const { return _join != JoinType::none; }  ///< True if this is a JOIN

        void addJoinCondition(unique_ptr<ExprNode>);  ///< Sets/adds an `ON` condition to a JOIN

        /// True if this is a regular collection, not an UNNEST or a table-based index.
        bool isCollection() const { return !isUnnest() && !isIndex(); }

        bool isUnnest() const { return _unnest || _tempUnnest; }  ///< True if this is an UNNEST expression

        bool isIndex() const { return !_indexedNodes.empty(); }  ///< True if this is a table-based index

        std::vector<checked_ptr<IndexedNode>> const& indexedNodes() const { return _indexedNodes; }

        IndexType   indexType() const;
        string_view indexedExpressionJSON() const;

        void checkIndexUsage() const;

        bool matchPath(KeyPath& path) const override;
        void visitChildren(ChildVisitor const&) override;
        void writeSQL(SQLWriter&) const override;

      private:
        friend class SelectNode;
        void parseChildExprs(ParseContext&);
        void disambiguateColumnName(ParseContext&);

        void setUsesDeleted() { _usesDeleted = true; }

        void clearWeakRefs();

        string                                _scope;                  // Scope name, or empty for default
        string                                _collection;             // Collection name, or empty for default
        string                                _columnName;             // Name to use if used as result column
        string                                _tableName;              // SQLite table name (set by caller)
        JoinType                              _join = JoinType::none;  // Type of JOIN, or none
        unique_ptr<ExprNode>                  _joinOn;                 // "ON ..." predicate
        Value                                 _tempOn;                 // Temporarily holds source of _joinOn
        unique_ptr<ExprNode>                  _unnest;                 // "UNNEST ..." source expression
        Value                                 _tempUnnest;             // Temporarily holds source of _unnest
        std::vector<checked_ptr<IndexedNode>> _indexedNodes;           // IndexedNodes using this index, if any
        bool                                  _usesDeleted = false;    // True if exprs refer to deleted docs
    };

    /** A `SELECT` statement, whether top-level or nested. */
    class SelectNode : public ExprNode {
      public:
        explicit SelectNode(Value v, ParseContext& ctx) { parse(v, ctx); }

        virtual ~SelectNode();

        /// All the sources: collections, joins, unnested expressions, table-based indexes.
        std::vector<unique_ptr<SourceNode>> const& sources() const { return _sources; }

        /// All the projections (returned values.)
        std::vector<unique_ptr<WhatNode>> const& what() const { return _what; }

        /// The WHERE clause.
        ExprNode* C4NULLABLE where() const { return _where.get(); }

        /// The LIMIT clause.
        ExprNode* C4NULLABLE limit() const { return _limit.get(); }

        /// True if the query uses aggregate functions, `GROUP BY` or `DISTINCT`.
        /// Set during postprocessing.
        bool isAggregate() const { return _isAggregate; }

        /// The number of columns that will automatically be prepended before the ones in `what()`.
        /// (This is a kludge introduced by the FTS query design ages ago.)
        unsigned numPrependedColumns() const { return _numPrependedColumns; }

        void visitChildren(ChildVisitor const&) override;
        void postprocess(ParseContext&) override;
        void writeSQL(SQLWriter&) const override;

      protected:
        SelectNode() = default;
        void parse(Value, ParseContext&);
        void addAllSourcesTo(std::vector<SourceNode*>&) const;

      private:
        void   registerAlias(AliasedNode*, ParseContext&);
        void   addSource(std::unique_ptr<SourceNode>, ParseContext&);
        void   addIndexes(ParseContext&);
        void   addIndexForNode(IndexedNode&, ParseContext&);
        string makeIndexAlias() const;
        void   writeFTSColumns(SQLWriter&, fleece::delimiter&) const;

        std::vector<unique_ptr<SourceNode>>           _sources;                      // The sources (FROM exprs)
        std::vector<unique_ptr<WhatNode>>             _what;                         // The WHAT expressions
        unique_ptr<ExprNode>                          _where;                        // The WHERE expression
        checked_ptr<SourceNode>                       _from;                         // Main source (also in _sources)
        std::vector<unique_ptr<ExprNode>>             _groupBy;                      // The GROUP BY expressions
        unique_ptr<ExprNode>                          _having;                       // The HAVING expression
        std::vector<pair<unique_ptr<ExprNode>, bool>> _orderBy;                      // The ORDER BY expressions
        unique_ptr<ExprNode>                          _limit;                        // The LIMIT expression
        unique_ptr<ExprNode>                          _offset;                       // The OFFSET expression
        std::vector<checked_ptr<SelectNode>>          _nestedSelects;                // SELECTs nested in this one
        bool                                          _distinct            = false;  // True if DISTINCT is given
        bool                                          _isAggregate         = false;  // Uses aggregate fns?
        unsigned                                      _numPrependedColumns = 0;      // Columns added by FTS
    };

    /** The root node of a query; a simple subclass of `SelectNode`. */
    class QueryNode final : public SelectNode {
      public:
        explicit QueryNode(string_view json);
        explicit QueryNode(Value);

        /// Returns sources of all SELECTs including nested ones.
        std::vector<SourceNode*> allSources() const;

      private:
        RetainedValue _root;  // Ensures the Fleece object tree is retained.
    };

}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
