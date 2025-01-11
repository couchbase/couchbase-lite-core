//
// IndexedNodes.hh
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
#include "SelectNodes.hh"

C4_ASSUME_NONNULL_BEGIN

namespace litecore::qt {
    class IndexSourceNode;

    /*  QueryTranslator syntax nodes for functions that use table-based indexes,
        namely FTS and vector search. */


    /** Abstract base class of Nodes using a table-based (FTS, vector) index.
        Each instance is associated with a SourceNode added to the query, representing the index. */
    class IndexedNode : public ExprNode {
      public:
        IndexType indexType() const { return _type; }

        /// A unique identifier of the indexed expression, used to match it with an IndexSourceNode.
        string_view indexID() const { return _indexID; }

        /// The collection being searched.
        SourceNode* C4NULLABLE sourceCollection() const { return _sourceCollection; }

        /// IndexSourceNode representing the SQLite index table.
        IndexSourceNode* C4NULLABLE indexSource() const { return _indexSource; }

        /// Sets which SQLite index table is being queried; called by `SelectNode::addIndexForNode`
        virtual void setIndexSource(IndexSourceNode* source, SelectNode* select, ParseContext& ctx) {
            _indexSource = source;
            _select      = select;
        }

        /// True if this is just an accessory function that requires another function to define the index search.
        /// Currently only true for a RankNode.
        bool isAuxiliary() const { return _isAuxiliary; }

        /// Writes SQL for the index table name (or SELECT expression)
        virtual void writeSourceTable(SQLWriter& ctx, string_view tableName) const;

      protected:
        IndexedNode(IndexType type) : _type(type) {}

        void setIndexedExpression(ExprNode*);

        IndexType const             _type;                 // Index type
        ExprNode*                   _indexedExpr;          // The indexed expression (usually a doc property)
        string_view                 _indexID;              // Expression/property that's indexed
        SourceNode* C4NULLABLE      _sourceCollection{};   // The collection being queried
        IndexSourceNode* C4NULLABLE _indexSource{};        // Source representing the index
        SelectNode* C4NULLABLE      _select{};             // The containing SELECT statement
        bool                        _isAuxiliary = false;  // True if this is an auxiliary expression (e.g. `RANK()`)
    };

    /** Abstract base class of FTS nodes. */
    class FTSNode : public IndexedNode {
      protected:
        FTSNode(Array::iterator& args, ParseContext&, const char* name);

        void writeIndex(SQLWriter&) const;
    };

    /** An FTS `match()` function call. */
    class MatchNode final : public FTSNode {
      public:
        MatchNode(Array::iterator& args, ParseContext&);

        void writeSQL(SQLWriter&) const override;
        void visitChildren(ChildVisitor const&) override;

      private:
        ExprNode* _searchString;
    };

    /** An FTS `rank()` function call. */
    class RankNode final : public FTSNode {
      public:
        RankNode(Array::iterator& args, ParseContext& ctx);

        OpFlags opFlags() const override { return kOpNumberResult; }

        void writeSQL(SQLWriter&) const override;
    };


#ifdef COUCHBASE_ENTERPRISE

    /** A `vector_distance(property, vector, [metric], [numProbes], [accurate])` function call. */
    class VectorDistanceNode final : public IndexedNode {
      public:
        static constexpr unsigned kMaxMaxResults = 10000;

        VectorDistanceNode(Array::iterator& args, ParseContext& ctx);

        string_view metric() const;

        OpFlags opFlags() const override { return kOpNumberResult; }

        void visitChildren(ChildVisitor const&) override;
        void setIndexSource(IndexSourceNode* source, SelectNode* select, ParseContext& ctx) override;
        void writeSourceTable(SQLWriter& ctx, string_view tableName) const override;
        void writeSQL(SQLWriter&) const override;

      private:
        ExprNode* _vector;            // The vector being queried
        int       _metric;            // Distance metric (actually vectorsearch::Metric)
        unsigned  _numProbes = 0;     // Number of probes, or 0 for default
        bool      _simple    = true;  // True if this is a simple (non-hybrid) query
    };

    /** A `prediction()` function call that uses an index. */
    class PredictionNode final : public IndexedNode {
      public:
        static ExprNode* parse(Array::iterator args, ParseContext&);

        void writeSQL(SQLWriter&) const override;

      private:
        PredictionNode(Array::iterator& args, ParseContext& ctx, string_view indexID);

        const char* C4NULLABLE _subProperty{};
    };

#endif

#pragma mark - INDEX SOURCE:

    /** A table-based index, implicitly added to the tree by an `IndexedNode` (FTS or vector.) */
    class IndexSourceNode final : public SourceNode {
      public:
        explicit IndexSourceNode(IndexedNode*, string_view alias, ParseContext& ctx);

        IndexType   indexType() const;
        string_view indexID() const;

        bool matchesNode(IndexedNode const*) const;

        IndexedNode* indexedNode() const { return _indexedNode; }

        void addIndexedNode(IndexedNode*);

      private:
        friend class SelectNode;
        void checkIndexUsage() const;

        IndexedNode* _indexedNode;  // Main IndexedNode using this index
    };

}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
