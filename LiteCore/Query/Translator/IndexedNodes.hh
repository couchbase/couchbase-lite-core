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
        IndexType indexType() const                 {return _type;}
        string_view property() const                {return _property;}
        bool isIndexOwner() const                   {return _isIndexOwner;}
        SourceNode* sourceCollection() const        {return _sourceCollection;}

        void setIndexSource(SourceNode* s)          {_indexSource = s;}
        SourceNode* indexSource() const             {return _indexSource;}

        virtual void writeSourceTable(SQLWriter& ctx, string_view tableName) const;

    protected:
        IndexedNode(Array::iterator &args, ParseContext&, IndexType, const char* name, bool isOwner);
        void writeIndex(SQLWriter&) const;

        IndexType   _type;                          // Index type
        string      _property;                      // Collection property that's indexed
        SourceNode* _sourceCollection = nullptr;    // The collection containing the index
        SourceNode* _indexSource = nullptr;         // Source representing the index
        bool        _isIndexOwner;
    };


    /** An FTS `match()` function call. */
    class MatchNode final : public IndexedNode {
    public:
        MatchNode(Array::iterator &args, ParseContext&);

        void writeSQL(SQLWriter&) const override;
        void visit(Visitor const& visitor, unsigned depth = 0) override;
        void rewriteChildren(const Rewriter&) override;
#if DEBUG
        string description() const override         {return "match()";}
#endif
    private:
        unique_ptr<ExprNode> _searchString;
    };


    /** An FTS `rank()` function call. */
    class RankNode final : public IndexedNode {
    public:
        RankNode(Array::iterator &args, ParseContext& ctx);
        void writeSQL(SQLWriter&) const override;
        OpFlags opFlags() const override;
#if DEBUG
        string description() const override         {return "rank()";}
#endif
    };


#ifdef COUCHBASE_ENTERPRISE
    

    /** A `vector_match()` function call. */
    class VectorMatchNode final : public IndexedNode {
    public:
        VectorMatchNode(Array::iterator &args, ParseContext&);

        void writeSourceTable(SQLWriter& ctx, string_view tableName) const override;
        void writeSQL(SQLWriter&) const override;
        void visit(Visitor const& visitor, unsigned depth = 0) override;
        void rewriteChildren(const Rewriter&) override;
#if DEBUG
        string description() const override         {return "vector_match()";}
#endif
    private:
        unique_ptr<ExprNode> _vector, _maxResults;
    };


    /** A `vector_distance()` function call. */
    class VectorDistanceNode final : public IndexedNode {
    public:
        VectorDistanceNode(Array::iterator &args, ParseContext& ctx);
        void writeSQL(SQLWriter&) const override;
        OpFlags opFlags() const override;
        void writeSourceTable(SQLWriter& ctx, string_view tableName) const override;
#if DEBUG
        string description() const override         {return "vector_distance()";}
#endif
    };

#endif
}
