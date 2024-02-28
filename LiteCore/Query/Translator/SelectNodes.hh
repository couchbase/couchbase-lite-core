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

namespace fleece {
    class delimiter;
}
namespace litecore::qt {
    class IndexedNode;


    /** A named Node -- abstract base of WhatNode and SourceNode. */
    class AliasedNode : public Node {
    public:
        string const& alias() const                 {return _alias;}
        bool hasExplicitAlias() const               {return _hasExplicitAlias;}

        virtual bool matchPath(KeyPath& path) const;
        
    protected:
        string  _alias;                     // Name I'm referred to by
        bool    _hasExplicitAlias = false;  // _alias was given by an AS property
    };


    /** A projection returned by a query; an item in the WHAT clause. */
    class WhatNode final : public AliasedNode {
    public:
        WhatNode(Value, ParseContext&);
        explicit WhatNode(unique_ptr<ExprNode> expr)                 :_expr(std::move(expr)) { }

        string columnName() const;
        void setColumnName(string s)                    {_columnName = std::move(s);}

        void visit(Visitor const& visitor, unsigned depth = 0) override;
        void rewriteChildren(const Rewriter&) override;
        void writeSQL(SQLWriter&) const override;
        bool matchPath(KeyPath& path) const override;
#if DEBUG
        string description() const override;
#endif

    private:
        friend class SelectNode;
        void parseChildExprs(ParseContext&);
        void ensureUniqueColumnName(std::unordered_set<string>& columnNames);

        unique_ptr<ExprNode> _expr;                 // The value to return
        Value                _tempChild;            // Temporarily holds source of _expr
        string               _columnName;           // Computed name of the result column
        bool                 _parsingExpr = false;  // kludgy temporary flag
    };


    /** An item in the FROM clause: a collection, join, unnested expression, or table-based index.*/
    class SourceNode final : public AliasedNode {
    public:
        explicit SourceNode(Dict, ParseContext&);
        explicit SourceNode(string_view alias);
        explicit SourceNode(IndexedNode&);

        string const& scope() const                 {return _scope;}
        string const& collection() const            {return _collection;}
        bool usesDeletedDocs() const                {return _usesDeleted;}
        string_view tableName() const               {return _tableName;}
        void setTableName(string_view name)         {_tableName = name;}
        string asColumnName() const                 {return _columnName;}

        JoinType joinType() const                   {return _join;}
        bool isJoin() const                         {return _join != JoinType::none;}
        bool isCollection() const                   {return !isUnnest() && !isIndex();}
        bool isUnnest() const                       {return _unnest || _tempUnnest;}

        bool isIndex() const                        {return !_indexedNodes.empty();}
        IndexType indexType() const;
        string_view indexedProperty() const;
        std::vector<IndexedNode*> const& indexedNodes() const   {return _indexedNodes;}
        void checkIndexUsage() const;

        void setUsesDeleted()                       {_usesDeleted = true;}

        bool matchPath(KeyPath& path) const override;

        void visit(Visitor const& visitor, unsigned depth = 0) override;
        void rewriteChildren(const Rewriter&) override;
        void writeSQL(SQLWriter&) const override;
#if DEBUG
        string description() const override;
#endif

    private:
        friend class SelectNode;
        void parseChildExprs(ParseContext&);
        void disambiguateColumnName(ParseContext&);

        string                  _scope;                 // Scope name, or empty for default
        string                  _collection;            // Collection name, or empty for default
        string                  _columnName;            // Name to use if used as result column
        string                  _tableName;             // SQLite table name (set by caller)
        JoinType                _join = JoinType::none; // Type of JOIN, or none
        unique_ptr<ExprNode>    _joinOn;                // "ON ..." predicate
        Value                   _tempOn;                // Temporarily holds source of _joinOn
        unique_ptr<ExprNode>    _unnest;                // "UNNEST ..." source expression
        Value                   _tempUnnest;            // Temporarily holds source of _unnest
        std::vector<IndexedNode*> _indexedNodes;        // IndexedNodes using this index, if any
        bool                    _usesDeleted = false;   // True if exprs refer to deleted docs
    };


    /** A SELECT statement, whether top-level or nested. */
    class SelectNode : public ExprNode {
    public:
        explicit SelectNode(Value v, ParseContext& ctx)             {parse(v, ctx);}

        std::vector<unique_ptr<SourceNode>> const& sources() const  {return _sources;}
        std::vector<unique_ptr<WhatNode>> const& what() const       {return _what;}
        bool isAggregate() const                                    {return _isAggregate;}
        unsigned numPrependedColumns() const                        {return _numPrependedColumns;}

        void visit(Visitor const& visitor, unsigned depth = 0) override;
        void rewriteChildren(const Rewriter&) override;
        Node* postprocess(ParseContext&) override;
        void writeSQL(SQLWriter&) const override;
#if DEBUG
        string description() const override                            {return "SELECT";}
#endif

    protected:
        SelectNode() = default;
        void parse(Value, ParseContext&);
        void addAllSourcesTo(std::vector<SourceNode*>&) const;
    private:
        void registerAlias(AliasedNode*, ParseContext&);
        void addSource(std::unique_ptr<SourceNode>, ParseContext&);
        void addIndexes(ParseContext&);
        void addIndexForNode(IndexedNode&, ParseContext&);
        string makeIndexAlias() const;
        void writeFTSColumns(SQLWriter&, fleece::delimiter&) const;

        unique_ptr<ExprNode>                _where;             // The WHERE expression
        std::vector<unique_ptr<WhatNode>>   _what;              // The WHAT expressions
        std::vector<unique_ptr<SourceNode>> _sources;           // The sources (FROM exprs)
        SourceNode*                         _from = nullptr;    // Main source (also in _sources)
        std::vector<unique_ptr<ExprNode>>   _groupBy;           // The GROUP BY expressions
        unique_ptr<ExprNode>                _having;            // The HAVING expression
        std::vector<pair<unique_ptr<ExprNode>,bool>> _orderBy;  // The ORDER BY expressions
        unique_ptr<ExprNode>                _limit;             // The LIMIT expression
        unique_ptr<ExprNode>                _offset;            // The OFFSET expression
        std::vector<SelectNode*>            _nestedSelects;     // SELECTs nested in this one
        bool                                _distinct = false;  // True if DISTINCT is given
        bool                                _isAggregate = false; // Uses aggregate fns?
        unsigned                            _numPrependedColumns = 0; // Columns added by FTS
    };


    /** The root node of a query; a simple subclass of SelectNode. */
    class QueryNode final : public SelectNode {
    public:
        explicit QueryNode(string_view json);
        explicit QueryNode(Value);

        /// Returns sources of all SELECTs including nested ones.
        std::vector<SourceNode*> allSources() const;

    private:
        RetainedValue _root;            // Ensures the Fleece object tree is retained.
    };

}
