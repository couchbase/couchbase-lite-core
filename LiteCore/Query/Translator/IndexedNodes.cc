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
    constexpr const char* kOwnerFnName[3] = {nullptr, "MATCH", "APPROX_VECTOR_DISTANCE"};

    IndexedNode::IndexedNode(IndexType type) : _type(type) {
        DebugAssert(type != IndexType::none);
    }

    void IndexedNode::writeSourceTable(SQLWriter& ctx, string_view tableName) const { ctx << sqlIdentifier(tableName); }

#pragma mark - FTS:

    // Initializes using the index name as the first argument (for FTS)
    FTSNode::FTSNode(Array::iterator& args, ParseContext& ctx, const char* name)
        : IndexedNode(IndexType::FTS) {
        slice pathStr = args[0].asString();
        require(!pathStr.empty(), "first arg of %s() must be an index name", name);
        KeyPath path = parsePath(pathStr);
        // Find the source collection and property name/path:
        _sourceCollection = dynamic_cast<SourceNode*>(resolvePropertyPath(path, ctx, true));
        require(_sourceCollection, "unknown source collection for %s()", name);
        require(_sourceCollection->isCollection(), "invalid source collection for %s()", name);
        require(path.count() > 0, "missing property after collection alias in %s()", name);
        _indexExpressionJSON = string(path.toString());
    }

    void FTSNode::writeIndex(SQLWriter& sql) const {
        Assert(_indexSource, "FTSNode's indexSource wasn't set");
        sql << sqlIdentifier(_indexSource->alias()) << '.' << sqlIdentifier(_indexSource->tableName());
    }

    MatchNode::MatchNode(Array::iterator& args, ParseContext& ctx)
        : FTSNode(args, ctx, "MATCH"), _searchString(ExprNode::parse(args[1], ctx)) {}

    void MatchNode::visitChildren(ChildVisitor const& visitor) { visitor(*_searchString); }

    void MatchNode::writeSQL(SQLWriter& ctx) const {
        Parenthesize p(ctx, kMatchPrecedence);
        writeIndex(ctx);
        ctx << " MATCH " << _searchString;
    }

    RankNode::RankNode(Array::iterator& args, ParseContext& ctx) : FTSNode(args, ctx, "RANK") {_isAuxiliary = true;}

    void RankNode::writeSQL(SQLWriter& ctx) const {
        ctx << "rank(matchinfo(";
        writeIndex(ctx);
        ctx << "))";
    }

#pragma mark - VECTOR:


#ifdef COUCHBASE_ENTERPRISE

    // A SQLite vector MATCH expression; used by VectorDistanceNode to add a join condition.
    class VectorMatchNode final : public ExprNode {
      public:
        VectorMatchNode(SourceNode* index, ExprNode* vector) : _index(index), _vector(vector) {}

        void writeSQL(SQLWriter& sql) const override {
            sql << sqlIdentifier(_index->alias()) << ".vector MATCH encode_vector(" << _vector << ")";
        }

      private:
        SourceNode* _index;
        ExprNode*   _vector;
    };

    VectorDistanceNode::VectorDistanceNode(Array::iterator& args, ParseContext& ctx)
        : IndexedNode(IndexType::vector) {
        _indexedExpr = parse(args[0], ctx);

        // Determine which collection the vector is based on:
        _indexedExpr->visit([&](Node& n, unsigned depth) {
            if ( SourceNode* src = n.source() ) {
                require(_sourceCollection == nullptr || _sourceCollection == src,
                        "1st argument (vector) to APPROX_VECTOR_DISTANCE may only refer to a single collection");
                _sourceCollection = src;
            }
        });
        if ( !_sourceCollection ) _sourceCollection = ctx.from;

        // Create the JSON expression used to locate the index:
        _indexExpressionJSON = args[0].toJSON(false, true);
        bool fixed           = false;
        if ( string const& alias = _sourceCollection->alias(); !alias.empty() ) {
            fixed = replace(_indexExpressionJSON, "[\"." + alias + ".", "[\".");
        }
        if ( !fixed ) {
            if ( string prefix = _sourceCollection->collection(); !prefix.empty() ) {
                // A kludge to remove the collection name from the path:
                if ( string const& scope = _sourceCollection->scope(); !scope.empty() ) prefix = scope + "." + prefix;
                replace(_indexExpressionJSON, "[\"." + prefix + ".", "[\".");
            }
        }

        _vector = ExprNode::parse(args[1], ctx);

        if ( Value metricVal = args[2] ) {
            _metric = requiredString(metricVal, "3rd argument (metric) to APPROX_VECTOR_DISTANCE");
        }

        if ( Value numProbesVal = args[3] ) {
            require(numProbesVal.isInteger(), "4th argument (numProbes) to APPROX_VECTOR_DISTANCE must be an integer");
            auto numProbes = numProbesVal.asInt();
            require(numProbes > 0 && numProbes < UINT_MAX,
                    "4th argument (numProbes) to APPROX_VECTOR_DISTANCE out of range");
            _numProbes = unsigned(numProbes);
        }

        if ( Value accurate = args[4] ) {
            require(accurate.type() == kFLBoolean,
                    "5th argument (accurate) to APPROX_VECTOR_DISTANCE must be `false`, if given");
            require(accurate.asBool() == false, "APPROX_VECTOR_DISTANCE does not support 'accurate'=true");
        }
    }

    void VectorDistanceNode::setIndexSource(SourceNode* source, SelectNode* select) {
        IndexedNode::setIndexSource(source, select);
        _simple = [&] {
            // Returns true if the WHERE clause does _not_ require a hybrid query,
            // i.e. if it's nonexistent or consists only of a test that APPROX_VECTOR_DISTANCE() is less than something.
            auto where = _select->where();
            if ( !where ) return true;
            auto opNode = dynamic_cast<OpNode*>(where);
            if ( !opNode ) return false;
            ExprNode* expr;
            slice     op = opNode->op().name;
            if ( op == "<" || op == "<=" ) expr = opNode->operand(0);
            else if ( op == ">" || op == ">=" )
                expr = opNode->operand(1);
            else
                return false;
            return expr && dynamic_cast<VectorDistanceNode*>(expr);
        }();
        if ( !_simple && source->indexedNodes().size() < 2 ) {
            // Add a join condition "idx.vector MATCH _vector"
            DebugAssert(source->indexedNodes().front() == this);
            source->addJoinCondition(make_unique<VectorMatchNode>(source, _vector.get()));
        }

        // Disallow distance within an OR because it can lead to incorrect results:
        bool withinOR = false;
        for ( Node const* n = parent(); n; n = n->parent() ) {
            if ( auto op = dynamic_cast<OpNode const*>(n); op && op->op().name == "OR" ) {
                withinOR = true;
            } else if ( auto sel = dynamic_cast<SelectNode const*>(n) ) {
                require(!withinOR, "APPROX_VECTOR_DISTANCE can't be used within an OR in a WHERE clause");
                break;
            }
        }
    }

    void VectorDistanceNode::writeSourceTable(SQLWriter& sql, string_view tableName) const {
        if ( _simple ) {
            // In a "simple" vector match, run the vector query as a nested SELECT:
            sql << "(SELECT docid, distance FROM " << sqlIdentifier(tableName) << " WHERE vector MATCH encode_vector("
                << _vector << ")";
            if ( _numProbes > 0 ) { sql << " AND vectorsearch_probes(vector, " << _numProbes << ")"; }
            Node const* limit = _select->limit();
            require(limit, "a LIMIT must be given when using APPROX_VECTOR_DISTANCE()");
            sql << " LIMIT " << limit << ")";
        } else {
            IndexedNode::writeSourceTable(sql, tableName);
        }
    }

    void VectorDistanceNode::visitChildren(ChildVisitor const& visitor) {
        visitor(*_indexedExpr);
        visitor(*_vector);
    }

    void VectorDistanceNode::writeSQL(SQLWriter& ctx) const {
        ctx << sqlIdentifier(_indexSource->alias()) << ".distance";
    }

#endif


#pragma mark - ADDITIONS TO SOURCE & SELECT NODES:

    SourceNode::SourceNode(IndexedNode& node)
        : _scope(node.sourceCollection()->_scope)
        , _collection(node.sourceCollection()->_collection)
        , _join(JoinType::inner) {}

    void SourceNode::checkIndexUsage() const {
        if ( indexType() == IndexType::FTS ) {
            // There must be exactly one MATCH node:
            size_t n = std::count_if(_indexedNodes.begin(), _indexedNodes.end(),
                                     [](IndexedNode* node) { return !dynamic_cast<FTSNode*>(node)->isAuxiliary(); });
            if ( n == 0 ) fail("RANK() cannot be used without MATCH()");
            else if ( n > 1 )
                fail("Sorry, multiple MATCHes of the same property are not allowed");
        }
    }

    template <class T>
    static bool aliasExists(string const& alias, vector<T> const& _sources) {
        return _sources.end() != std::find_if(_sources.begin(), _sources.end(), [&](auto& s) {
                   return 0 == compareIgnoringCase(alias, s->alias());
               });
    }

    string SelectNode::makeIndexAlias() const {
        int    n = 1;
        string alias;
        do { alias = format("<idx%d>", n++); } while ( aliasExists(alias, _sources) || aliasExists(alias, _what) );
        return alias;
        // (Searching ctx.aliases would be easier, but it doesn't contain index sources)
    }

    // As part of postprocessing, locates FTS and vector indexed expressions and adds corresponding JOINed tables.
    void SelectNode::addIndexes(ParseContext& ctx) {
        unsigned validToDepth = 0;
        if ( _where ) {
            _where->visit([&](Node& node, unsigned depth) {
                validToDepth = std::min(validToDepth, depth);
                if ( auto op = dynamic_cast<OpNode*>(&node); op && op->op().name == "AND" ) {
                    if ( depth == validToDepth ) ++validToDepth;
                } else if ( auto ind = dynamic_cast<IndexedNode*>(&node); ind && !ind->isAuxiliary() ) {
                    // Add JOINs on FTS indexes for MATCH or RANK nodes:
                    if ( ind->indexType() == IndexType::FTS ) {
                        require(depth == validToDepth, "%s can only appear at top-level, or in a top-level AND",
                                kOwnerFnName[int(ind->indexType())]);
                    }
                    addIndexForNode(*ind, ctx);
                }
            });
        }

        visit([&](Node& node, unsigned depth) {
            if ( auto ind = dynamic_cast<IndexedNode*>(&node); ind && !ind->indexSource() ) {
                require(ind->indexType() != IndexType::FTS || ind->isAuxiliary(),
                        "a %s is not allowed outside the WHERE clause", kOwnerFnName[int(ind->indexType())]);
                addIndexForNode(*ind, ctx);
            }
        });

        // Then check that there's exactly one function call that 'owns' each index:
        for ( auto& source : _sources ) source->checkIndexUsage();
    }

    /// Adds a SourceNode for an IndexedNode, or finds an existing one.
    /// Sets the source as its indexSource.
    void SelectNode::addIndexForNode(IndexedNode& node, ParseContext& ctx) {
        DebugAssert(node.indexType() != IndexType::none);
        DebugAssert(!node.indexExpressionJSON().empty());

        // Look for an existing index source:
        SourceNode* indexSrc = nullptr;
        for ( auto& s : _sources ) {
            if ( s->indexType() == node.indexType() && s->indexedProperty() == node.indexExpressionJSON()
                 && s->collection() == node.sourceCollection()->collection()
                 && s->scope() == node.sourceCollection()->scope() ) {
                indexSrc = s.get();
                break;
            }
        }

        if ( !indexSrc ) {
            // None found; need to create it:
            auto source    = make_unique<SourceNode>(node);
            indexSrc       = source.get();
            source->_alias = makeIndexAlias();
            // Create the join condition:
            auto cond = make_unique<OpNode>(*lookupOp("=", 2));
            cond->addArg(make_unique<RawSQLNode>("\"" + source->_alias + "\".docid"));
            cond->addArg(make_unique<MetaNode>(MetaProperty::rowid, node.sourceCollection()));
            source->_joinOn = std::move(cond);

            addSource(std::move(source), ctx);

            if ( node.indexType() == IndexType::FTS && !_isAggregate ) {
                // writeSQL is going to prepend extra columns for an FTS index:
                _numPrependedColumns = std::max(_numPrependedColumns, 1u) + 1;
            }
        }

        indexSrc->_indexedNodes.push_back(&node);
        node.setIndexSource(indexSrc, this);
    }

    // When FTS is used in a query, invisible columns are prepended that help the Query API
    // find the matched text. This was a bad design but we're stuck with it...
    void SelectNode::writeFTSColumns(SQLWriter& ctx, delimiter& comma) const {
        if ( !_isAggregate ) {
            for ( auto& src : _sources ) {
                if ( src->indexType() == IndexType::FTS ) {
                    if ( comma.count() == 0 ) ctx << comma << sqlIdentifier(_from->alias()) << ".rowid";
                    ctx << comma << "offsets(" << sqlIdentifier(src->alias()) << "." << sqlIdentifier(src->tableName())
                        << ')';
                }
            }
        }
    }
}  // namespace litecore::qt
