//
// IndexedNodes.cc
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
#include "VectorIndexSpec.hh"
#include "SelectNodes.hh"
#include "SQLWriter.hh"
#include "TranslatorUtils.hh"

namespace litecore::qt {
    using namespace std;
    using namespace fleece;

    // indexed by IndexType:
    constexpr const char* kOwnerFnName[2] = {"MATCH", "APPROX_VECTOR_DISTANCE"};

#pragma mark - FTS:

    // Initializes using the index name as the first argument (for FTS)
    FTSNode::FTSNode(Array::iterator& args, ParseContext& ctx, const char* name) : IndexedNode(IndexType::FTS) {
        slice pathStr = args[0].asString();
        require(!pathStr.empty(), "first arg of %s() must be an index name", name);
        KeyPath path = parsePath(pathStr);
        // Find the source collection and property name/path:
        auto source = dynamic_cast<SourceNode*>(resolvePropertyPath(path, ctx, true));
        require(source, "unknown source collection for %s()", name);
        require(source->isCollection(), "invalid source collection for %s()", name);
        require(path.count() > 0, "missing property after collection alias in %s()", name);
        _sourceCollection    = source;
        _indexExpressionJSON = string(path.toString());
    }

    void FTSNode::writeSourceTable(SQLWriter& ctx, string_view tableName) const {
        require(!tableName.empty(), "missing FTS index");
        ctx << sqlIdentifier(tableName);
    }

    void FTSNode::writeIndex(SQLWriter& sql) const {
        Assert(_indexSource, "FTSNode's indexSource wasn't set");
        sql << sqlIdentifier(_indexSource->alias()) << '.' << sqlIdentifier(_indexSource->tableName());
    }

    MatchNode::MatchNode(Array::iterator& args, ParseContext& ctx)
        : FTSNode(args, ctx, "MATCH"), _searchString(ExprNode::parse(args[1], ctx)) {}

    void MatchNode::visitChildren(ChildVisitor const& visitor) { visitor(_searchString); }

    void MatchNode::writeSQL(SQLWriter& ctx) const {
        Parenthesize p(ctx, kMatchPrecedence);
        writeIndex(ctx);
        ctx << " MATCH " << _searchString;
    }

    RankNode::RankNode(Array::iterator& args, ParseContext& ctx) : FTSNode(args, ctx, "RANK") { _isAuxiliary = true; }

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
        VectorMatchNode(IndexSourceNode* index, ExprNode* vector) : _index(index), _vector(vector) {}

        void writeSQL(SQLWriter& sql) const override {
            sql << sqlIdentifier(_index->alias()) << ".vector MATCH encode_vector(" << _vector << ")";
        }

      private:
        IndexSourceNode* _index;
        ExprNode*        _vector;
    };

    VectorDistanceNode::VectorDistanceNode(Array::iterator& args, ParseContext& ctx)
        : IndexedNode(IndexType::vector), _indexedExpr(parse(args[0], ctx)) {
        // Determine which collection the vector is based on:
        SourceNode* source = nullptr;
        _indexedExpr->visitTree([&](Node& n, unsigned /*depth*/) {
            if ( SourceNode* nodeSource = n.source() ) {
                require(source == nullptr || source == nodeSource,
                        "1st argument (vector) to APPROX_VECTOR_DISTANCE may only refer to a single collection");
                source = nodeSource;
            }
        });
        require(source, "unknown source collection for APPROX_VECTOR_DISTANCE()");
        _sourceCollection = source;

        // Create the JSON expression used to locate the index:
        string indexExpr(args[0].toJSON(false, true));
        bool   fixed = false;
        if ( string_view alias = _sourceCollection->alias(); !alias.empty() ) {
            fixed = replace(indexExpr, "[\"." + string(alias) + ".", "[\".");
        }
        if ( !fixed ) {
            if ( string prefix = string(_sourceCollection->collection()); !prefix.empty() ) {
                // A kludge to remove the collection name from the path:
                if ( string_view scope = _sourceCollection->scope(); !scope.empty() )
                    prefix = string(scope) + "." + prefix;
                replace(indexExpr, "[\"." + prefix + ".", "[\".");
            }
        }
        _indexExpressionJSON = ctx.newString(indexExpr);

        _vector = ExprNode::parse(args[1], ctx);

        if ( slice metricName = optionalString(args[2], "3rd argument (metric) to APPROX_VECTOR_DISTANCE") ) {
            if ( auto metric = vectorsearch::MetricNamed(metricName) ) _metric = int(metric.value());
            else
                fail("invalid metric name '%.*s' for APPROX_VECTOR_DISTANCE", FMTSLICE(metricName));
        } else {
            _metric = int(vectorsearch::Metric::Default);
        }

        if ( Value numProbesVal = args[3] ) {
            require(numProbesVal.isInteger(), "4th argument (numProbes) to APPROX_VECTOR_DISTANCE must be an integer");
            auto numProbes = numProbesVal.asInt();
            require(numProbes > 0 && numProbes < UINT_MAX,
                    "4th argument (numProbes) to APPROX_VECTOR_DISTANCE out of range");
            _numProbes = unsigned(numProbes);
        }

        if ( Value accurate = args[4] ) {
            require(accurate.type() == kFLBoolean, "5th argument (accurate) to APPROX_VECTOR_DISTANCE must be boolean");
            require(accurate.asBool() == false, "APPROX_VECTOR_DISTANCE does not support 'accurate'=true");
        }
    }

    string_view VectorDistanceNode::metric() const { return vectorsearch::NameOfMetric(vectorsearch::Metric(_metric)); }

    void VectorDistanceNode::setIndexSource(IndexSourceNode* source, SelectNode* select, ParseContext& ctx) {
        IndexedNode::setIndexSource(source, select, ctx);
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

        if ( !_simple && source->indexedNode() == this ) {
            // Hybrid query: add a join condition "idx.vector MATCH _vector"
            source->addJoinCondition(new (ctx) VectorMatchNode(source, _vector), ctx);
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
        require(!tableName.empty(), "missing vector index");
        if ( _simple ) {
            // In a "simple" vector match, run the vector query as a nested SELECT:
            sql << "(SELECT docid, distance FROM " << sqlIdentifier(tableName) << " WHERE vector MATCH encode_vector("
                << _vector << ")";
            if ( _numProbes > 0 ) { sql << " AND vectorsearch_probes(vector, " << _numProbes << ")"; }
            Node const* limit = _select->limit();
            require(limit, "a LIMIT must be given when using APPROX_VECTOR_DISTANCE()");
            sql << " LIMIT " << limit << ")";
        } else {
            sql << sqlIdentifier(tableName);
        }
    }

    void VectorDistanceNode::visitChildren(ChildVisitor const& visitor) { visitor(_indexedExpr)(_vector); }

    void VectorDistanceNode::writeSQL(SQLWriter& ctx) const {
        ctx << sqlIdentifier(_indexSource->alias()) << ".distance";
    }

#endif


#pragma mark - INDEX SOURCE:

    IndexSourceNode::IndexSourceNode(IndexedNode* node, string_view alias, ParseContext& ctx)
        : SourceNode(SourceType::index, node->sourceCollection()->scope(), node->sourceCollection()->collection(),
                     JoinType::inner)
        , _indexedNode{node} {
        _alias = ctx.newString(alias);
        // Create the join condition:
        auto cond = new (ctx) OpNode(*lookupOp("=", 2));
        cond->addArg(new (ctx) RawSQLNode("\"" + string(_alias) + "\".docid", ctx));
        cond->addArg(new (ctx) MetaNode(MetaProperty::rowid, node->sourceCollection()));
        addJoinCondition(cond, ctx);
    }

    bool IndexSourceNode::matchesNode(const IndexedNode* node) const {
        return _indexedNode->indexType() == node->indexType()
               && _indexedNode->indexExpressionJSON() == node->indexExpressionJSON()
               && collection() == node->sourceCollection()->collection()
               && scope() == node->sourceCollection()->scope();
    }

    IndexType IndexSourceNode::indexType() const { return _indexedNode->indexType(); }

    string_view IndexSourceNode::indexedExpressionJSON() const { return _indexedNode->indexExpressionJSON(); }

    void IndexSourceNode::addIndexedNode(IndexedNode* node) {
        Assert(node != _indexedNode && node->indexType() == _indexedNode->indexType());
        if ( _indexedNode->isAuxiliary() ) {
            _indexedNode = node;
        } else if ( !node->isAuxiliary() ) {
            if ( node->indexType() == IndexType::FTS )
                fail("Sorry, multiple MATCHes of the same property are not allowed");
        }
    }

    void IndexSourceNode::checkIndexUsage() const {
        require(!_indexedNode->isAuxiliary(), "RANK() cannot be used without MATCH()");
    }

#pragma mark - ADDITIONS TO SELECTNODE:

    template <class T>
    static bool aliasExists(string const& alias, List<T> const& list) {
        for ( auto n : list )
            if ( 0 == compareIgnoringCase(alias, string(n->alias())) ) return true;
        return false;
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
            _where->visitTree([&](Node& node, unsigned depth) {
                validToDepth = std::min(validToDepth, depth);
                if ( auto op = dynamic_cast<OpNode*>(&node); op && op->op().name == "AND" ) {
                    if ( depth == validToDepth ) ++validToDepth;
                } else if ( auto ind = dynamic_cast<IndexedNode*>(&node); ind && !ind->isAuxiliary() ) {
                    // Add JOINs on FTS indexes for MATCH or RANK nodes:
                    if ( ind->indexType() == IndexType::FTS ) {
                        require(depth == validToDepth, "%s can only appear at top-level, or in a top-level AND",
                                kOwnerFnName[int(ind->indexType())]);
                    }
                    addIndexForNode(ind, ctx);
                }
            });
        }

        visitTree([&](Node& node, unsigned /*depth*/) {
            if ( auto ind = dynamic_cast<IndexedNode*>(&node); ind && !ind->indexSource() ) {
                require(ind->indexType() != IndexType::FTS || ind->isAuxiliary(),
                        "a %s is not allowed outside the WHERE clause", kOwnerFnName[int(ind->indexType())]);
                addIndexForNode(ind, ctx);
            }
        });

        // Then check that there's exactly one function call that 'owns' each index:
        for ( SourceNode* source : _sources ) {
            if ( auto index = dynamic_cast<IndexSourceNode*>(source) ) index->checkIndexUsage();
        }
    }

    /// Adds a SourceNode for an IndexedNode, or finds an existing one.
    /// Sets the source as its indexSource.
    void SelectNode::addIndexForNode(IndexedNode* node, ParseContext& ctx) {
        DebugAssert(!node->indexExpressionJSON().empty());

        // Look for an existing index source:
        IndexSourceNode* indexSrc = nullptr;
        for ( SourceNode* s : _sources ) {
            if ( auto ind = dynamic_cast<IndexSourceNode*>(s); ind && ind->matchesNode(node) ) {
                indexSrc = ind;
                break;
            }
        }

        if ( indexSrc ) {
            indexSrc->addIndexedNode(node);
        } else {
            // No source found; need to create it:
            auto source = new (ctx) IndexSourceNode(node, makeIndexAlias(), ctx);
            indexSrc    = source;
            addSource(source, ctx);

            if ( node->indexType() == IndexType::FTS && !_isAggregate ) {
                // writeSQL is going to prepend extra columns for an FTS index:
                _numPrependedColumns = std::max(_numPrependedColumns, uint8_t(1u)) + 1;
            }
        }

        node->setIndexSource(indexSrc, this, ctx);
    }

    // When FTS is used in a query, invisible columns are prepended that help the Query API
    // find the matched text. This was a bad design but we're stuck with it...
    void SelectNode::writeFTSColumns(SQLWriter& ctx, delimiter& comma) const {
        if ( !_isAggregate ) {
            for ( SourceNode* src : _sources ) {
                if ( auto ind = dynamic_cast<IndexSourceNode*>(src) ) {
                    if ( ind->indexType() == IndexType::FTS ) {
                        if ( comma.count() == 0 ) ctx << comma << sqlIdentifier(from()->alias()) << ".rowid";
                        ctx << comma << "offsets(" << sqlIdentifier(ind->alias()) << "."
                            << sqlIdentifier(ind->tableName()) << ')';
                    }
                }
            }
        }
    }

}  // namespace litecore::qt
