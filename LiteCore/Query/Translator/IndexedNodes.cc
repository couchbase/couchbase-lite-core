//
// FTSNodes.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "IndexedNodes.hh"
#include "Delimiter.hh"
#include "Error.hh"
#include "SelectNodes.hh"
#include "SQLWriter.hh"
#include "TranslatorUtils.hh"

namespace litecore::qt {
    using namespace std;
    using namespace fleece;


    // These are indexed by IndexType:
    constexpr const char* kOwnerFnName[3] = {
        nullptr, "MATCH", "VECTOR_MATCH"
    };
    constexpr const char* kNonOwnerFnName[3] = {
        nullptr, "MATCH", "VECTOR_MATCH"
    };


    IndexedNode::IndexedNode(Array::iterator &args, 
                             ParseContext& ctx,
                             IndexType type,
                             const char* name,
                             bool isOwner)
    :_type(type)
    ,_isIndexOwner(isOwner)
    {
        DebugAssert(type != IndexType::none);
        slice pathStr = args[0].asString();
        require(!pathStr.empty(), "first arg of %s() must be an index name", name);
        KeyPath path = parsePath(pathStr);
        // Find the source collection and property name/path:
        _sourceCollection = dynamic_cast<SourceNode*>(resolvePropertyPath(path, ctx, true));
        require(_sourceCollection, "unknown source collection for %s()", name);
        require(_sourceCollection->isCollection(), "invalid source collection for %s()", name);
        require(path.count() > 0, "missing property after collection alias in %s()", name);
        _property = string(path.toString());
    }


    void IndexedNode::writeIndex(SQLWriter& ctx) const {
        Assert(_indexSource, "IndexedNode's indexSource wasn't set");
        ctx << sqlIdentifier(_indexSource->alias()) << '.'
            << sqlIdentifier(_indexSource->tableName());
    }


    void IndexedNode::writeSourceTable(SQLWriter& ctx, string_view tableName) const {
        ctx << sqlIdentifier(tableName);
    }


#pragma mark - FTS:


    MatchNode::MatchNode(Array::iterator &args, ParseContext& ctx)
    :IndexedNode(args, ctx, IndexType::FTS, "match", true)
    ,_searchString(ExprNode::parse(args[1], ctx))
    { }


    void MatchNode::visitChildren(ChildVisitor const& visitor) {
        visitor(*_searchString);
    }


    void MatchNode::writeSQL(SQLWriter &ctx) const {
        Parenthesize p(ctx, kMatchPrecedence);
        writeIndex(ctx);
        ctx << " MATCH " << _searchString;
    }


    RankNode::RankNode(Array::iterator &args, ParseContext& ctx)
    :IndexedNode(args, ctx, IndexType::FTS, "rank", false)
    { }


    OpFlags RankNode::opFlags() const            {return kOpNumberResult;}


    void RankNode::writeSQL(SQLWriter &ctx) const {
        ctx << "rank(matchinfo(";
        writeIndex(ctx);
        ctx << "))";
    }


#pragma mark - VECTOR:


#ifdef COUCHBASE_ENTERPRISE

    VectorMatchNode::VectorMatchNode(Array::iterator &args, ParseContext& ctx)
    :IndexedNode(args, ctx, IndexType::vector, "vector_match", true)
    ,_vector(ExprNode::parse(args[1], ctx))
    {
        if (args.count() > 2)
            _maxResults = ExprNode::parse(args[2], ctx);
    }


    void VectorMatchNode::visitChildren(ChildVisitor const& visitor) {
        visitor(*_vector);
        if (_maxResults)
            visitor(*_maxResults);
    }


    void VectorMatchNode::writeSourceTable(SQLWriter& ctx, string_view tableName) const {
        ctx << "(SELECT docid, distance FROM " << sqlIdentifier(tableName)
            << " WHERE vector LIKE encode_vector(" << _vector << ")";
        if (_maxResults)
            ctx << " LIMIT " << _maxResults;
        ctx << ")";
    }


    void VectorMatchNode::writeSQL(SQLWriter &ctx) const {
        // I don't need to do anything myself; the logic is all in the index source.
        ctx << "true";
    }


    VectorDistanceNode::VectorDistanceNode(Array::iterator &args, ParseContext& ctx)
    :IndexedNode(args, ctx, IndexType::vector, "vector_distance", false)
    { }


    OpFlags VectorDistanceNode::opFlags() const            {return kOpNumberResult;}


    void VectorDistanceNode::writeSourceTable(SQLWriter& ctx, string_view tableName) const {
        // Find the VectorMatchNode and delegate to it:
        for (IndexedNode* n : _indexSource->indexedNodes())
            if (auto match = dynamic_cast<VectorMatchNode*>(n))
                return match->writeSourceTable(ctx, tableName);
        fail("vector_distance() cannot be used without a corresponding vector_search()");
    }


    void VectorDistanceNode::writeSQL(SQLWriter &ctx) const {
        ctx << sqlIdentifier(_indexSource->alias()) << ".distance";
    }

#endif
    

#pragma mark - ADDITIONS TO SOURCE & SELECT NODES:


    SourceNode::SourceNode(IndexedNode& node)
    :_scope(node.sourceCollection()->_scope)
    ,_collection(node.sourceCollection()->_collection)
    ,_join(JoinType::inner)
    { }


    void SourceNode::checkIndexUsage() const {
        if (auto t = int(indexType())) {
            // There must be exactly one "owner" IndexedNode, i.e. MATCH or VECTOR_MATCH:
            size_t n = std::count_if(_indexedNodes.begin(), _indexedNodes.end(),
                                     [](IndexedNode* node) { return node->isIndexOwner(); });
            if (n == 0)
                fail("%s() cannot be used without %s()", kNonOwnerFnName[t], kOwnerFnName[t]);
            else if (n > 1)
                fail("Sorry, multiple %ses of the same property are not allowed", kOwnerFnName[t]);
        }
    }


    template <class T>
    static bool aliasExists(string const& alias, vector<T> const& _sources) {
        return _sources.end() != std::find_if(_sources.begin(), _sources.end(), [&](auto& s) {
            return 0 == compareIgnoringCase(alias, s->alias());
        });
    }


    string SelectNode::makeIndexAlias() const {
        int n = 1;
        string alias;
        do {
            alias = format("<idx%d>", n++);
        } while (aliasExists(alias, _sources) || aliasExists(alias, _what));
        return alias;
        // (Searching ctx.aliases would be easier, but it doesn't contain index sources)
    }


    void SelectNode::addIndexes(ParseContext& ctx) {
        unsigned validToDepth = 0;
        if (_where) {
            _where->visit([&](Node& node, unsigned depth) {
                validToDepth = std::min(validToDepth, depth);
                if (auto op = dynamic_cast<OpNode*>(&node); op && op->op().name == "AND") {
                    if (depth == validToDepth)
                        ++validToDepth;
                } else if (auto ind = dynamic_cast<IndexedNode*>(&node); ind && ind->isIndexOwner()) {
                    // Add JOINs on FTS indexes for MATCH or RANK nodes:
                    require(depth == validToDepth,
                            "%s can only appear at top-level, or in a top-level AND",
                            kOwnerFnName[int(ind->indexType())]);
                    addIndexForNode(*ind, ctx);
                }
            });
        }

        visit([&](Node& node, unsigned depth) {
            if (auto ind = dynamic_cast<IndexedNode*>(&node); ind && !ind->indexSource()) {
                require(!ind->isIndexOwner(), "a %s is not allowed outside the WHERE clause",
                        kOwnerFnName[int(ind->indexType())]);
                addIndexForNode(*ind, ctx);
            }
        });

        // Then check that there's exactly one function call that 'owns' each index:
        for (auto& source : _sources)
            source->checkIndexUsage();
    }


    /// Adds a SourceNode for an IndexedNode, or finds an existing one.
    /// Sets the source as its indexSource.
    void SelectNode::addIndexForNode(IndexedNode& node, ParseContext& ctx) {
        DebugAssert(node.indexType() != IndexType::none);
        DebugAssert(!node.property().empty());

        // Look for an existing index source:
        SourceNode* indexSrc = nullptr;
        for (auto& s : _sources) {
            if (s->indexType() == node.indexType() && s->indexedProperty() == node.property()
                    && s->collection() == node.sourceCollection()->collection()
                    && s->scope() == node.sourceCollection()->scope()) {
                indexSrc = s.get();
                break;
            }
        }

        if (!indexSrc) {
            // None found; need to create it:
            auto source = make_unique<SourceNode>(node);
            indexSrc = source.get();
            source->_alias = makeIndexAlias();
            // Create the join condition:
            auto cond = make_unique<OpNode>(*lookupOp("=", 2));
            cond->addArg(make_unique<RawSQLNode>("\"" + source->_alias + "\".docid"));
            cond->addArg(make_unique<MetaNode>(MetaProperty::rowid, node.sourceCollection()));
            source->_joinOn = std::move(cond);

            addSource(std::move(source), ctx);

            if (node.indexType() == IndexType::FTS && !_isAggregate) {
                // writeSQL is going to prepend extra columns for an FTS index:
                _numPrependedColumns = std::max(_numPrependedColumns, 1u) + 1;
            }
        }

        indexSrc->_indexedNodes.push_back(&node);
        node.setIndexSource(indexSrc);
    }


    // When FTS is used in a query, invisible columns are prepended that help the Query API
    // find the matched text. This was a bad design but we're stuck with it...
    void SelectNode::writeFTSColumns(SQLWriter &ctx, delimiter& comma) const {
        if (!_isAggregate) {
            for (auto& src : _sources) {
                if (src->indexType() == IndexType::FTS) {
                    if (comma.count() == 0)
                        ctx << comma << sqlIdentifier(_from->alias()) << ".rowid";
                    ctx << comma << "offsets("
                    << sqlIdentifier(src->alias()) << "."
                    << sqlIdentifier(src->tableName()) << ')';
                }
            }
        }
    }
}
