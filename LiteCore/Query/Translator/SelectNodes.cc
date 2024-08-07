//
// SelectNodes.cc
//
// Copyright © 2024 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "SelectNodes.hh"
#include "IndexedNodes.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "TranslatorUtils.hh"
#include <unordered_set>

namespace litecore::qt {
    using namespace fleece;
    using namespace std;


#pragma mark - WHAT:

    bool AliasedNode::matchPath(KeyPath& path) const {
        if ( path.count() > 0 && path.get(0).first.caseEquivalent(_alias) ) {
            // 1st component of the path equals my alias:
            path.dropComponents(1);
            return true;
        }
        return false;
    }

    WhatNode::WhatNode(Value v, ParseContext& ctx) {
        if ( Array a = v.asArray(); a[0].asString() == "AS" ) {
            // Handle ["AS", expr, alias]:
            require(a.count() == 3, "AS must have 2 operands");
            _alias = requiredString(a[2], "name in AS");
            require(!_alias.empty(), "invalid identifier '%s' in AS", _alias.c_str());
            _hasExplicitAlias = true;
            _tempChild        = a[1];
        } else {
            _tempChild = v;
        }
    }

    void WhatNode::parseChildExprs(ParseContext& ctx) {
        DebugAssert(!_expr);
        if ( slice prop = _tempChild.asString(); prop && !_hasExplicitAlias ) {
            // Convenience shortcut: interpret a string in a WHAT as a property path
            _expr = PropertyNode::parse(prop, nullptr, ctx);
        } else {
            _parsingExpr = true;
            _expr        = ExprNode::parse(_tempChild, ctx);
            _parsingExpr = false;
        }
        _expr->setParent(this);
        _tempChild = nullptr;
    }

    bool WhatNode::matchPath(KeyPath& path) const {
        // Don't allow myself to be used as an alias by my own expression (during parseChildExprs)
        return !_parsingExpr && AliasedNode::matchPath(path);
    }

    void WhatNode::ensureUniqueColumnName(unordered_set<string>& columnNames) {
        string curName = columnName();
        if ( curName.empty() || !columnNames.insert(curName).second ) {
            if ( hasExplicitAlias() ) {
                DebugAssert(!curName.empty());
                fail("duplicate column name '%s'", curName.c_str());
            } else {
                unsigned count = 1;
                string   name;
                do {
                    if ( curName.empty() ) name = format("$%u", count);
                    else
                        name = format("%s #%u", curName.c_str(), count + 1);
                    ++count;
                } while ( !columnNames.insert(name).second );
                setColumnName(name);
            }
        }
    }

    string WhatNode::columnName() const {
        if ( !_columnName.empty() ) return _columnName;
        else if ( _hasExplicitAlias )
            return _alias;
        else
            return _expr->asColumnName();
    }

    void WhatNode::visitChildren(ChildVisitor const& visitor) { visitor(*_expr); }

#pragma mark - SOURCE:

    SourceNode::SourceNode(string_view alias) {
        _alias            = alias;
        _hasExplicitAlias = true;
        _columnName       = alias;
    }

    SourceNode::SourceNode(fleece::Dict dict, ParseContext& ctx) {
        bool explicitScope = false;
        if ( slice scope = optionalString(getCaseInsensitive(dict, "SCOPE"), "SCOPE") ) {
            explicitScope = true;
            if ( scope == "_" || scope == kDefaultScopeName ) scope = "";
            _scope = scope;
        }
        bool explicitCollection = false;
        if ( slice collection = optionalString(getCaseInsensitive(dict, "COLLECTION"), "COLLECTION") ) {
            explicitCollection = true;
            if ( collection == "_" || collection == kDefaultCollectionName ) {
                _collection = "";
                _columnName = collection;
            } else {
                _collection = collection;
                if ( auto dot = _collection.find('.'); dot != string::npos ) {
                    // COLLECTION contains both a scope and collection name:
                    require(_scope.empty(), "if SCOPE is given, COLLECTION cannot contain a scope");
                    _scope      = _collection.substr(0, dot);
                    _collection = _collection.substr(dot + 1);
                    if ( _scope.empty() || _collection.empty() )
                        fail("`%.*s` is not a valid collection name", FMTSLICE(collection));
                }
                _columnName = _collection;
            }
        }

        if ( !explicitScope && !explicitCollection && ctx.from ) {
            _scope      = ctx.from->_scope;
            _collection = ctx.from->_collection;
        }

        if ( slice alias = optionalString(getCaseInsensitive(dict, "AS"), "AS") ) {
            require(!alias.empty(), "invalid alias 'AS %.*s'", FMTSLICE(alias));
            _hasExplicitAlias = true;
            _alias            = alias;
            replace(_alias, "\\", "");
            _columnName = _alias;
        } else {
            require(explicitCollection, "missing AS and COLLECTION in FROM item");
            if ( !_scope.empty() ) _alias = _scope + ".";
            _alias += _columnName;
        }

        if ( slice join = optionalString(getCaseInsensitive(dict, "JOIN"), "JOIN") ) {
            _join = lookupJoin(join);
            require(_join != JoinType::none, "invalid JOIN type");
        }

        _tempUnnest = getCaseInsensitive(dict, "UNNEST");
        if ( _tempUnnest ) { require(_join == JoinType::none, "UNNEST cannot accept a JOIN clause"); }

        _tempOn = getCaseInsensitive(dict, "ON");
        if ( _tempOn ) {
            // (Don't parse the expression yet; it might refer to aliases of later sources.)
            require(_join != JoinType::cross, "CROSS JOIN cannot accept an ON clause");
            require(!_tempUnnest, "UNNEST cannot accept an ON clause");
            if ( _join == JoinType::none ) _join = JoinType::inner;
        } else {
            require(_join == JoinType::none || _join == JoinType::cross, "missing ON for JOIN");
        }
    }

    void SourceNode::parseChildExprs(ParseContext& ctx) {
        // Now parse the ON or UNNEST expression:
        if ( _tempOn ) {
            _joinOn = ExprNode::parse(_tempOn, ctx);
            _joinOn->setParent(this);
            _tempOn = nullptr;
        }
        if ( _tempUnnest ) {
            _unnest = ExprNode::parse(_tempUnnest, ctx);
            _unnest->setParent(this);
            _tempUnnest = nullptr;
        }
    }

    bool SourceNode::matchPath(KeyPath& path) const {
        if ( AliasedNode::matchPath(path) ) return true;
        if ( path.count() >= 2 && !_hasExplicitAlias ) {
            // If my alias is "scope.collection", see if that matches 1st 2 components:
            string_view scope      = _scope.empty() ? kDefaultScopeName : _scope;
            string_view collection = _collection.empty() ? kDefaultCollectionName : _collection;
            if ( path.get(0).first.caseEquivalent(scope) && path.get(1).first.caseEquivalent(collection) ) {
                path.dropComponents(2);
                return true;
            }
        }
        return false;
    }

    void SourceNode::disambiguateColumnName(ParseContext& ctx) {
        if ( isCollection() && !_scope.empty() && _columnName.find('.') == string::npos ) {
            // Should I prepend my scope to my column name to disambiguate it?
            for ( SourceNode* src : ctx.sources ) {
                if ( src != this && src->_columnName == _columnName ) {
                    _columnName = _scope + "." + _columnName;
                    break;
                }
            }
        }
    }

    void SourceNode::addJoinCondition(unique_ptr<ExprNode> expr) {
        if ( !_joinOn ) {
            _joinOn = std::move(expr);
        } else {
            auto conjunction = make_unique<OpNode>(*lookupOp("AND", 2));
            conjunction->addArg(std::move(_joinOn));
            conjunction->addArg(std::move(expr));
            _joinOn = std::move(conjunction);
        }
    }

    IndexType SourceNode::indexType() const { return isIndex() ? _indexedNodes[0]->indexType() : IndexType::none; }

    string_view SourceNode::indexedProperty() const { return isIndex() ? _indexedNodes[0]->property() : ""; }

    void SourceNode::visitChildren(ChildVisitor const& visitor) {
        if ( _joinOn ) visitor(*_joinOn);
        if ( _unnest ) visitor(*_unnest);
    }

#pragma mark - SELECT:

    // Parses a LIMIT or OFFSET value. If it's a literal, it's checked for validity.
    // Otherwise it's wrapped in `GREATEST(x, 0)` to ensure a negative value means 0 not infinity.
    static unique_ptr<ExprNode> parseLimitOrOffset(Value val, ParseContext& ctx, const char* name) {
        auto expr = ExprNode::parse(val, ctx);
        if ( auto litNode = dynamic_cast<LiteralNode*>(expr.get()) ) {
            require(litNode->literal().isInteger() && litNode->literal().asInt() >= 0,
                    "%s must be a non-negative integer", name);
        } else {
            auto fixed = make_unique<FunctionNode>(lookupFn("GREATEST", 2));
            fixed->addArg(std::move(expr));
            fixed->addArg(make_unique<LiteralNode>(RetainedValue::newInt(0)));
            expr = std::move(fixed);
        }
        return expr;
    }

    void SelectNode::parse(Value v, ParseContext& ctx) {
        if ( ctx.select != nullptr ) {
            // About to parse a nested SELECT, with its own namespace; use a new ParseContext:
            ParseContext nestedCtx;
            parse(v, nestedCtx);
            ctx.select->_nestedSelects.push_back(this);
            return;
        }

        ctx.select = this;

        required(v, "SELECT statement");
        Dict select = v.asDict();
        if ( !select ) {
            if ( Array a = v.asArray(); a[0].asString().caseEquivalent("SELECT") ) {
                // Given an entire SELECT statement:
                select = requiredDict(a[1], "argument of SELECT");
            }
        }

        if ( select ) {
            // Parse FROM first, because it creates the SourceNodes that affect parsing of properties:
            if ( Value from = getCaseInsensitive(select, "FROM") ) {
                for ( Value i : requiredArray(from, "FROM") ) {
                    Dict item = requiredDict(i, "FROM item");
                    addSource(make_unique<SourceNode>(item, ctx), ctx);
                }
            }
            if ( _sources.empty() ) addSource(make_unique<SourceNode>("_doc"), ctx);
            require(_from, "missing a primary non-JOIN source");

            if ( Value what = getCaseInsensitive(select, "WHAT") ) {
                for ( Value w : requiredArray(what, "WHAT") ) {
                    auto whatNode = make_unique<WhatNode>(w, ctx);
                    if ( whatNode->hasExplicitAlias() ) registerAlias(whatNode.get(), ctx);
                    whatNode->setParent(this);
                    _what.push_back(std::move(whatNode));
                }
            }

            // After all alias are known, allow Source and What nodes to parse their expressions:
            for ( auto& source : _sources ) source->parseChildExprs(ctx);
            for ( auto& what : _what ) what->parseChildExprs(ctx);

            if ( Value where = getCaseInsensitive(select, "WHERE") ) {
                _where = ExprNode::parse(where, ctx);
                _where->setParent(this);
            }

            if ( Value order = getCaseInsensitive(select, "ORDER_BY") ) {
                for ( Value orderItem : requiredArray(order, "ORDER BY") ) {
                    bool ascending = true;
                    if ( auto a = orderItem.asArray(); a[0].asString().caseEquivalent("ASC") ) {
                        orderItem = a[1];
                    } else if ( a[0].asString().caseEquivalent("DESC") ) {
                        ascending = false;
                        orderItem = a[1];
                    }
                    auto orderNode = ExprNode::parse(orderItem, ctx);
                    orderNode->setParent(this);
                    _orderBy.emplace_back(std::move(orderNode), ascending);
                }
            }

            _distinct = getCaseInsensitive(select, "DISTINCT").asBool();

            if ( Value groupList = getCaseInsensitive(select, "GROUP_BY") ) {
                for ( Value groupItem : requiredArray(groupList, "GROUP BY") ) {
                    unique_ptr<ExprNode> group;
                    if ( slice prop = groupItem.asString() ) {
                        // Convenience shortcut: interpret a string in GROUP_BY as a property path
                        group = PropertyNode::parse(prop, nullptr, ctx);
                    } else {
                        group = ExprNode::parse(groupItem, ctx);
                    }
                    group->setParent(this);
                    _groupBy.emplace_back(std::move(group));
                }
            }

            if ( Value having = getCaseInsensitive(select, "HAVING") ) {
                _having = ExprNode::parse(having, ctx);
                _having->setParent(this);
            }
            if ( Value limit = getCaseInsensitive(select, "LIMIT") ) {
                _limit = parseLimitOrOffset(limit, ctx, "LIMIT");
                _limit->setParent(this);
            }
            if ( Value offset = getCaseInsensitive(select, "OFFSET") ) {
                _offset = parseLimitOrOffset(offset, ctx, "OFFSET");
                _offset->setParent(this);
            }

        } else {
            // If not given a Dict or ["SELECT",...], assume it's a WHERE clause:
            addSource(make_unique<SourceNode>("_doc"), ctx);
            _where = ExprNode::parse(v, ctx);
            _where->setParent(this);
        }

        if ( _what.empty() ) {
            // Default WHAT is id and sequence:
            _what.emplace_back(make_unique<WhatNode>(make_unique<MetaNode>(MetaProperty::id, _from)));
            _what.emplace_back(make_unique<WhatNode>(make_unique<MetaNode>(MetaProperty::sequence, _from)));
            _what[0]->setParent(this);
            _what[1]->setParent(this);
        }

        Assert(_from);
        Assert(ctx.from);
        Assert(!ctx.aliases.empty());
    }

    void SelectNode::registerAlias(AliasedNode* node, ParseContext& ctx) {
        slice alias    = node->alias();
        bool  inserted = ctx.aliases.insert({lowercase(string(alias)), node}).second;
        require(inserted, "duplicate alias '%.*s'", FMTSLICE(alias));
    }

    void SelectNode::addSource(unique_ptr<SourceNode> source, ParseContext& ctx) {
        if ( !source->isIndex() ) {
            registerAlias(source.get(), ctx);
            if ( source->isCollection() && !source->isJoin() ) {
                require(!_from, "multiple non-join FROM items");
                _from    = source.get();
                ctx.from = _from;
            } else {
                require(_from, "first FROM item must be primary source");
            }
            ctx.sources.push_back(source.get());
        }
        source->setParent(this);
        _sources.emplace_back(std::move(source));
    }

    void SelectNode::addAllSourcesTo(std::vector<SourceNode*>& sources) const {
        for ( auto& s : _sources ) sources.push_back(s.get());
        for ( auto sel : _nestedSelects ) sel->addAllSourcesTo(sources);
    }

    void SelectNode::visitChildren(ChildVisitor const& visitor) {
        for ( auto& child : _what ) visitor(*child);
        for ( auto& child : _sources ) visitor(*child);
        if ( _where ) visitor(*_where);
        for ( auto& child : _orderBy ) visitor(*child.first);
        for ( auto& child : _groupBy ) visitor(*child);
        if ( _having ) visitor(*_having);
    }

    void SelectNode::postprocess(ParseContext& ctx) {
        Node::postprocess(ctx);

        _isAggregate = _distinct || !_groupBy.empty();

        visit([&](Node& node, unsigned depth) {
            if ( auto meta = dynamic_cast<MetaNode*>(&node) ) {
                // `meta()` calls that don't access any property implicity return the `deleted` property:
                auto prop = meta->property();
                if ( prop == MetaProperty::none || prop == MetaProperty::deleted ) meta->source()->setUsesDeleted();
            } else if ( auto fn = dynamic_cast<FunctionNode*>(&node) ) {
                // Look for aggregate functions:
                if ( fn->opFlags() & kOpAggregate ) _isAggregate = true;
            }
        });

        addIndexes(ctx);

        for ( auto& source : _sources ) {
            if ( !source->_usesDeleted && source->_collection.empty() && source->isCollection() ) {
                // The default collection may contain deleted documents in its main table,
                // so if the query didn't ask for deleted docs, add a condition to the WHERE
                // or ON clause that only passes live docs:
                auto                  m    = make_unique<MetaNode>(MetaProperty::_notDeleted, source.get());
                unique_ptr<ExprNode>& cond = source->isJoin() ? source->_joinOn : _where;
                if ( cond ) {
                    auto a = make_unique<OpNode>(*lookupOp("AND", 2));
                    a->addArg(std::move(cond));
                    a->addArg(std::move(m));
                    cond = std::move(a);
                } else {
                    cond = std::move(m);
                }
            }
        }

        // Ensure sources' column names are unique
        for ( auto& source : _sources ) source->disambiguateColumnName(ctx);

        {
            // Ensure the WHAT nodes have non-empty, unique column names.
            // In the first pass, make sure explicitly named columns (WHAT nodes) have unique names;
            // in the second pass, the other columns will add "$n" or " #n" to make themselves unique.
            unordered_set<string> columnNames;
            for ( int x = 1; x >= 0; --x ) {
                for ( auto& what : _what ) {
                    if ( what->hasExplicitAlias() == x ) what->ensureUniqueColumnName(columnNames);
                }
            }
        }
    }

    QueryNode::QueryNode(Value root) {
        _root = root;  // retain it for safety
        ParseContext ctx;
        parse(root, ctx);
        //dump(cout);  // <--in case of debugging emergency, break glass
        postprocess(ctx);
    }

    QueryNode::QueryNode(string_view json) : QueryNode(Doc::fromJSON(json).root()) {}

    vector<SourceNode*> QueryNode::allSources() const {
        vector<SourceNode*> sources;
        addAllSourcesTo(sources);
        return sources;
    }

}  // namespace litecore::qt
