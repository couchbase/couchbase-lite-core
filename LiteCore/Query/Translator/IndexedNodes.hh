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

C4_ASSUME_NONNULL_BEGIN

namespace litecore::qt {

    /*  QueryTranslator syntax nodes for functions that use table-based indexes,
        namely FTS and vector search. */


    /** Abstract base class of Nodes using a table-based (FTS, vector) index.
        Each instance is associated with a SourceNode added to the query, representing the index. */
    class IndexedNode : public ExprNode {
      public:
        IndexType indexType() const { return _type; }

        /// JSON of the indexed expression, usually a property
        string_view indexExpressionJSON() const { return _indexExpressionJSON; }

        /// The collection being searched.
        SourceNode* C4NULLABLE sourceCollection() const { return _sourceCollection; }

        /// SourceNode representing the SQLite index table.
        SourceNode* C4NULLABLE indexSource() const { return _indexSource; }

        /// Sets which SQLite index table is being queried; called by `SelectNode::addIndexForNode`
        virtual void setIndexSource(SourceNode* source, SelectNode* select) {
            _indexSource = source;
            _select      = select;
        }

        /// True if this is just an accessory function that requires another function to define the index search.
        /// Currently only true for a RankNode.
        bool isAuxiliary() const { return _isAuxiliary; }

        /// Writes SQL for the index table name (or SELECT expression)
        virtual void writeSourceTable(SQLWriter& ctx, string_view tableName) const;

      protected:
        IndexedNode(IndexType type);

        IndexType               _type;                 // Index type
        string                  _indexExpressionJSON;  // Expression/property that's indexed, as JSON
        checked_ptr<SourceNode> _sourceCollection;     // The collection being queried
        checked_ptr<SourceNode> _indexSource;          // Source representing the index
        checked_ptr<SelectNode> _select;               // The containing SELECT statement
        bool                    _isAuxiliary = false;  // True if this is an auxiliary expression (e.g. `RANK()`)
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
        unique_ptr<ExprNode> _searchString;
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
        VectorDistanceNode(Array::iterator& args, ParseContext& ctx);

        string_view metric() const { return _metric; }

        OpFlags opFlags() const override { return kOpNumberResult; }

        void visitChildren(ChildVisitor const&) override;
        void setIndexSource(SourceNode* source, SelectNode* select) override;
        void writeSourceTable(SQLWriter& ctx, string_view tableName) const override;
        void writeSQL(SQLWriter&) const override;

      private:
        unique_ptr<ExprNode> _indexedExpr;       // The indexed expression (usually a doc property)
        unique_ptr<ExprNode> _vector;            // The vector being queried
        string               _metric;            // Distance metric name, or empty for default
        unsigned             _numProbes = 0;     // Number of probes, or 0 for default
        bool                 _simple    = true;  // True if this is a simple (non-hybrid) query
    };

#endif
}  // namespace litecore::qt

C4_ASSUME_NONNULL_END
