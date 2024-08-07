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

namespace litecore::qt {

    /*  QueryTranslator syntax nodes for functions that use table-based indexes,
        namely FTS and vector search. */


    /** Abstract base class of Nodes using a table-based (FTS, vector) index.
        Each instance is associated with a SourceNode added to the query, representing the index. */
    class IndexedNode : public ExprNode {
      public:
        IndexType indexType() const { return _type; }

        string_view property() const { return _property; }

        /// True if this expression defines the indexed lookup, false if it's just an accessory function;
        /// i.e. true for `match()` but not for `rank()`.
        bool isIndexOwner() const { return _isIndexOwner; }

        /// The collection being searched.
        SourceNode* sourceCollection() const { return _sourceCollection; }

        /// SourceNode representing the SQLite index table.
        SourceNode* indexSource() const { return _indexSource; }

        /// Sets which SQLite index table is being queried; called by `SelectNode::addIndexForNode`
        virtual void setIndexSource(SourceNode* source, SelectNode* select) {
            _indexSource = source;
            _select      = select;
        }

        /// Writes SQL for the index table name (or SELECT expression)
        virtual void writeSourceTable(SQLWriter& ctx, string_view tableName) const;

      protected:
        IndexedNode(IndexType type, bool isOwner);
        void writeIndex(SQLWriter&) const;

        IndexType   _type;                        // Index type
        string      _property;                    // Collection property that's indexed
        SourceNode* _sourceCollection = nullptr;  // The collection being queried
        SourceNode* _indexSource      = nullptr;  // Source representing the index
        SelectNode* _select           = nullptr;
        bool        _isIndexOwner;  // False if this is an auxiliary expression (e.g. `RANK()`)
    };

    /** Abstract base class of FTS nodes. */
    class FTSNode : public IndexedNode {
      public:
      protected:
        FTSNode(Array::iterator& args, ParseContext&, const char* name, bool isOwner);
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
        void writeSQL(SQLWriter&) const override;

        OpFlags opFlags() const override { return kOpNumberResult; }
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
