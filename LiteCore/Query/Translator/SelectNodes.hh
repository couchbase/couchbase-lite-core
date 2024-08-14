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
        string_view alias() const { return _alias; }

        /// True if an alias was set explicitly by an `AS` expression.
        bool hasExplicitAlias() const { return _hasExplicitAlias; }

        /// If the path refers to this node (first component matches its alias), removes the first component and
        /// returns true. Else returns false.
        virtual bool matchPath(KeyPath& path) const;

      protected:
        string_view _alias;                     // Name I'm referred to by
        bool        _hasExplicitAlias = false;  // _alias was given by an AS property
    };

    /** A projection returned by a query; an item in the `WHAT` clause. Wraps an ExprNode. */
    class WhatNode final : public AliasedNode {
      public:
        WhatNode(Value, ParseContext&);

        explicit WhatNode(ExprNode* expr) { initChild(_expr, expr); }

        /// The name of the result column. If not explicitly set, makes one up based on the expression.
        string_view columnName() const;

        /// Explicitly sets the column name.
        void setColumnName(const char* s) { _columnName = s; }

        void visitChildren(ChildVisitor const&) override;
        void writeSQL(SQLWriter&) const override;
        bool matchPath(KeyPath& path) const override;

      private:
        friend class SelectNode;
        void parseChildExprs(ParseContext&);
        void ensureUniqueColumnName(std::unordered_set<string>& columnNames, ParseContext&);

        ExprNode* C4NULLABLE   _expr{};               // The expression being returned
        Value                  _tempChild;            // Temporarily holds source of _expr
        const char* C4NULLABLE _columnName{};         // Computed name of the result column
        bool                   _parsingExpr = false;  // kludgy temporary flag
    };

    enum class SourceType : uint8_t { collection, unnest, index };

    /** An item in the `FROM` clause: a collection, join, unnested expression, or table-based index. */
    class SourceNode : public AliasedNode {
      public:
        static SourceNode* parse(Dict, ParseContext&);
        explicit SourceNode(const char* alias);

        SourceType type() const { return _type; }

        bool isCollection() const { return _type == SourceType::collection; }

        string_view scope() const { return _scope; }  ///< Scope name, or empty if default

        string_view collection() const { return _collection; }  ///< Collection name, or empty if default

        bool usesDeletedDocs() const { return _usesDeleted; }  ///< True if exprs refer to deleted docs

        string_view asColumnName() const { return _columnName; }  ///< Name to use, if used as result column

        bool isJoin() const { return _join != JoinType::none; }  ///< True if this is a JOIN

        void addJoinCondition(ExprNode*, ParseContext&);  ///< Sets/adds an `ON` condition to a JOIN

        string_view tableName() const { return _tableName; }  ///< SQLite table name (set by QueryTranslator)

        void setTableName(const char* name) { _tableName = name; }  ///< Sets SQLite table name

        bool matchPath(KeyPath& path) const override;
        void visitChildren(ChildVisitor const&) override;
        void writeSQL(SQLWriter&) const override;

      protected:
        friend class SelectNode;

        SourceNode(SourceType type) : _type(type) {}

        SourceNode(Dict, ParseContext&);
        SourceNode(SourceType, string_view scope, string_view collection, JoinType join);
        void         parseAS(Dict, ParseContext&);
        virtual void parseChildExprs(ParseContext&);
        void         disambiguateColumnName(ParseContext&);
        void         writeASandON(SQLWriter& ctx) const;

        void setUsesDeleted() { _usesDeleted = true; }

      private:
        string_view          _scope;                  // Scope name, or empty for default
        string_view          _collection;             // Collection name, or empty for default
        string_view          _columnName;             // Name to use if used as result column
        string_view          _tableName;              // SQLite table name (set by caller)
        JoinType             _join = JoinType::none;  // Type of JOIN, or none
        ExprNode* C4NULLABLE _joinOn{};               // "ON ..." predicate
        Value                _tempOn;                 // Temporarily holds source of _joinOn
        bool                 _usesDeleted = false;    // True if exprs refer to deleted docs
        SourceType const     _type;
    };

    /** An UNNEST, i.e. a joined table representing the contents of an array in a document. */
    class UnnestSourceNode final : public SourceNode {
      public:
        UnnestSourceNode();
        UnnestSourceNode(Dict, ParseContext&);

        /// The expression referencing the document property that's the source of the data.
        ExprNode* unnestExpression() const { return _unnest; }

        /// Returns a string identifying the UNNEST expression; used for matching against an array index table.
        string unnestIdentifier() const;

        void visitChildren(ChildVisitor const&) override;
        void writeSQL(SQLWriter&) const override;

      private:
        void parseChildExprs(ParseContext&) override;

        ExprNode* C4NULLABLE _unnest{};                // "UNNEST ..." source expression
        Value                _unnestFleeceExpression;  // Parsed-JSON form of source expression
    };

    /** A `SELECT` statement, whether top-level or nested. */
    class SelectNode final : public ExprNode {
      public:
        explicit SelectNode(Value v, ParseContext& ctx) { parse(v, ctx); }

        /// All the sources: collections, joins, unnested expressions, table-based indexes.
        List<SourceNode> const& sources() const { return _sources; }

        /// The main collection data source, i.e. the first source after `FROM`.
        SourceNode* from() const { return _sources.front(); }

        /// All the projections (returned values.)
        List<WhatNode> const& what() const { return _what; }

        /// The WHERE clause.
        ExprNode* C4NULLABLE where() const { return _where; }

        /// The LIMIT clause.
        ExprNode* C4NULLABLE limit() const { return _limit; }

        /// True if the query uses aggregate functions, `GROUP BY` or `DISTINCT`.
        /// Set during postprocessing.
        bool isAggregate() const { return _isAggregate; }

        /// The number of columns that will automatically be prepended before the ones in `what()`.
        /// (This is a kludge introduced by the FTS query design ages ago.)
        unsigned numPrependedColumns() const { return _numPrependedColumns; }

        void visitChildren(ChildVisitor const&) override;
        void writeSQL(SQLWriter&) const override;

      private:
        void   parse(Value, ParseContext&);
        void   registerAlias(AliasedNode*, ParseContext&);
        void   addSource(SourceNode*, ParseContext&);
        void   addIndexes(ParseContext&);
        void   addIndexForNode(IndexedNode*, ParseContext&);
        string makeIndexAlias() const;
        void   writeFTSColumns(SQLWriter&, fleece::delimiter&) const;

        List<SourceNode>     _sources;                      // The sources (FROM exprs)
        List<WhatNode>       _what;                         // The WHAT expressions
        ExprNode* C4NULLABLE _where{};                      // The WHERE expression
        List<ExprNode>       _groupBy;                      // The GROUP BY expressions
        ExprNode* C4NULLABLE _having{};                     // The HAVING expression
        List<ExprNode>       _orderBy;                      // The ORDER BY expressions
        uint64_t             _orderDesc{};                  // Which items in _orderBy are DESC
        ExprNode* C4NULLABLE _limit{};                      // The LIMIT expression
        ExprNode* C4NULLABLE _offset{};                     // The OFFSET expression
        uint8_t              _numPrependedColumns = 0;      // Columns added by FTS
        bool                 _distinct            = false;  // True if DISTINCT is given
        bool                 _isAggregate         = false;  // Uses aggregate fns?
    };

}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
