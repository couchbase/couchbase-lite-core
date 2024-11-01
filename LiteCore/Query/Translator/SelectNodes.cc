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
#include "DataFile.hh"
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
            _alias = ctx.newString(requiredString(a[2], "name in AS"));
            require(!_alias.empty(), "invalid empty 'AS'");
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
            setChild(_expr, PropertyNode::parse(prop, nullptr, ctx));
        } else {
            _parsingExpr = true;
            setChild(_expr, ExprNode::parse(_tempChild, ctx));
            _parsingExpr = false;
        }
        _tempChild = nullptr;
    }

    bool WhatNode::matchPath(KeyPath& path) const {
        // Don't allow myself to be used as an alias by my own expression (during parseChildExprs)
        return !_parsingExpr && AliasedNode::matchPath(path);
    }

    void WhatNode::ensureUniqueColumnName(unordered_set<string>& columnNames, ParseContext& ctx) {
        string curName(columnName());
        if ( curName.empty() || !columnNames.insert(curName).second ) {
            if ( hasExplicitAlias() ) {
                DebugAssert(!curName.empty());
                fail("duplicate column name '%s'", curName.c_str());
            } else {
                unsigned count = 1;
                string   name;
                do {
                    if ( curName.empty() ) name = stringprintf("$%u", count);
                    else
                        name = stringprintf("%s #%u", curName.c_str(), count + 1);
                    ++count;
                } while ( !columnNames.insert(name).second );
                setColumnName(ctx.newString(name));
            }
        }
    }

    string_view WhatNode::columnName() const {
        if ( _columnName && *_columnName ) return _columnName;
        else if ( _hasExplicitAlias )
            return _alias;
        else
            return _expr->asColumnName();
    }

    void WhatNode::visitChildren(ChildVisitor const& visitor) { visitor(_expr); }

#pragma mark - SOURCE:

    SourceNode* SourceNode::parse(Dict dict, ParseContext& ctx) {
        if ( getCaseInsensitive(dict, "UNNEST") ) return new (ctx) UnnestSourceNode(dict, ctx);
        else
            return new (ctx) SourceNode(dict, ctx);
    }

    SourceNode::SourceNode(const char* alias) : SourceNode(SourceType::collection) {
        _alias            = alias;
        _hasExplicitAlias = true;
        _columnName       = alias;
    }

    SourceNode::SourceNode(SourceType type, string_view scope, string_view collection, JoinType join)
        : _scope(scope), _collection(collection), _join(join), _type(type) {}

    SourceNode::SourceNode(Dict dict, ParseContext& ctx) : SourceNode(SourceType::collection) {
        // Parse the SCOPE and COLLECTION properties:
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
                if ( auto dot = DataFile::findCollectionPathSeparator(_collection, 0); dot != string::npos ) {
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

        // Parse AS:
        parseAS(dict, ctx);
        if ( !_hasExplicitAlias ) {
            require(explicitCollection, "missing AS and COLLECTION in FROM item");
            string alias;
            if ( _scope.empty() ) alias = string(_alias);
            else
                alias = ctx.newString(string(_scope) + ".");
            _alias = ctx.newString(alias + string(_columnName));
        }

        // Parse JOIN and ON:
        if ( slice join = optionalString(getCaseInsensitive(dict, "JOIN"), "JOIN") ) {
            _join = lookupJoin(join);
            require(_join != JoinType::none, "invalid JOIN type");
        }
        _tempOn = getCaseInsensitive(dict, "ON");
        if ( _tempOn ) {
            // (Don't parse the expression yet; it might refer to aliases of later sources.)
            require(_join != JoinType::cross, "CROSS JOIN cannot accept an ON clause");
            if ( _join == JoinType::none ) _join = JoinType::inner;
        } else {
            require(_join == JoinType::none || _join == JoinType::cross, "missing ON for JOIN");
        }
    }

    void SourceNode::parseAS(Dict dict, ParseContext& ctx) {
        if ( slice alias = optionalString(getCaseInsensitive(dict, "AS"), "AS") ) {
            require(!alias.empty(), "invalid alias 'AS %.*s'", FMTSLICE(alias));
            _hasExplicitAlias = true;
            string aliasStr(alias);
            replace(aliasStr, "\\", "");
            _alias      = ctx.newString(aliasStr);
            _columnName = _alias;
        }
    }

    void SourceNode::parseChildExprs(ParseContext& ctx) {
        if ( _tempOn ) {
            setChild(_joinOn, ExprNode::parse(_tempOn, ctx));
            _tempOn = nullptr;
        }
    }

    bool SourceNode::matchPath(KeyPath& path) const {
        if ( AliasedNode::matchPath(path) ) return true;
        if ( path.count() >= 2 && !_hasExplicitAlias ) {
            // If my alias is "scope.collection", see if that matches 1st 2 components:
            string_view scope      = _scope.empty() ? string_view(kDefaultScopeName) : _scope;
            string_view collection = _collection.empty() ? string_view(kDefaultCollectionName) : _collection;
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
                    _columnName = ctx.newString(string(_scope) + "." + string(_columnName));
                    break;
                }
            }
        }
    }

    void SourceNode::addJoinCondition(ExprNode* expr, ParseContext& ctx) {
        if ( !_joinOn ) {
            _joinOn = expr;
        } else {
            auto conjunction = new (ctx) OpNode(*lookupOp("AND", 2));
            conjunction->addArg(_joinOn);
            conjunction->addArg(expr);
            _joinOn = conjunction;
        }
    }

    void SourceNode::visitChildren(ChildVisitor const& visitor) { visitor(_joinOn); }

#pragma mark - UNNEST SOURCE:

    UnnestSourceNode::UnnestSourceNode(Dict dict, ParseContext& ctx) : SourceNode(SourceType::unnest) {
        parseAS(dict, ctx);
        _unnestFleeceExpression = getCaseInsensitive(dict, "UNNEST");
        require(!getCaseInsensitive(dict, "JOIN") && !getCaseInsensitive(dict, "ON"),
                "UNNEST cannot accept a JOIN or ON clause");
    }

    // Creates a fake UNNEST table source for use by QueryTranslator::writeCreateIndex.
    UnnestSourceNode::UnnestSourceNode() : SourceNode(SourceType::unnest) {
        setTableName("FAKE_UNNEST");  // it needs a table name, else writeSQL() will barf
    }

    void UnnestSourceNode::parseChildExprs(ParseContext& ctx) {
        setChild(_unnest, ExprNode::parse(_unnestFleeceExpression, ctx));
    }

    string UnnestSourceNode::unnestIdentifier() const {
        DebugAssert(_unnest);
        if ( auto prop = dynamic_cast<PropertyNode*>(_unnest) ) {
            return string(prop->path());
        } else {
            return expressionIdentifier(_unnestFleeceExpression.asArray());
        }
    }

    void UnnestSourceNode::visitChildren(ChildVisitor const& visitor) {
        SourceNode::visitChildren(visitor);
        visitor(_unnest);
    }

#pragma mark - SELECT:

    // Parses a LIMIT or OFFSET value. If it's a literal, it's checked for validity.
    // Otherwise it's wrapped in `GREATEST(x, 0)` to ensure a negative value means 0 not infinity.
    static ExprNode* parseLimitOrOffset(Value val, ParseContext& ctx, const char* name) {
        auto expr = ExprNode::parse(val, ctx);
        if ( auto litNode = dynamic_cast<LiteralNode*>(expr) ) {
            optional<int64_t> i = litNode->asInt();
            require(i, "%s must be an integer", name);
            if ( i.value() < 0 ) litNode->setInt(0);
        } else {
            auto fixed = new (ctx) FunctionNode(lookupFn("GREATEST", 2));
            fixed->addArg(expr);
            fixed->addArg(new (ctx) LiteralNode(0));
            expr = fixed;
        }
        return expr;
    }

    void SelectNode::parse(Value v, ParseContext& ctx) {
        if ( ctx.select != nullptr ) {
            // About to parse a nested SELECT, with its own namespace; use a new ParseContext:
            ParseContext nestedCtx(ctx);
            parse(v, nestedCtx);
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
                    addSource(SourceNode::parse(item, ctx), ctx);
                }
            }
            if ( _sources.empty() ) {
                // For historical reasons, if no primary source is given,
                // we add one for the default collection aliased `_doc`.
                addSource(new (ctx) SourceNode("_doc"), ctx);
            }
            require(!_sources.empty(), "missing a primary non-JOIN source");

            // Parse the WHAT clause (projections):
            if ( Value what = getCaseInsensitive(select, "WHAT") ) {
                for ( Value w : requiredArray(what, "WHAT") ) {
                    auto whatNode = new (ctx) WhatNode(w, ctx);
                    if ( whatNode->hasExplicitAlias() ) registerAlias(whatNode, ctx);
                    addChild(_what, whatNode);
                }
            }

            // After all aliases are known, allow Source and What Nodes to parse their expressions:
            for ( SourceNode* source : _sources ) source->parseChildExprs(ctx);
            for ( WhatNode* what : _what ) what->parseChildExprs(ctx);

            // Parse the WHERE clause:
            if ( Value where = getCaseInsensitive(select, "WHERE") ) { setChild(_where, ExprNode::parse(where, ctx)); }

            if ( Value order = getCaseInsensitive(select, "ORDER_BY") ) {
                for ( Value orderItem : requiredArray(order, "ORDER BY") ) {
                    bool descending = false;
                    if ( auto a = orderItem.asArray(); a[0].asString().caseEquivalent("ASC") ) {
                        orderItem = a[1];
                    } else if ( a[0].asString().caseEquivalent("DESC") ) {
                        descending = true;
                        orderItem  = a[1];
                    }
                    if ( descending ) _orderDesc |= (1 << _orderBy.size());
                    addChild(_orderBy, ExprNode::parse(orderItem, ctx));
                }
            }

            _distinct = getCaseInsensitive(select, "DISTINCT").asBool();

            if ( Value groupList = getCaseInsensitive(select, "GROUP_BY") ) {
                for ( Value groupItem : requiredArray(groupList, "GROUP BY") ) {
                    ExprNode* group;
                    if ( slice prop = groupItem.asString() ) {
                        // Convenience shortcut: interpret a string in GROUP_BY as a property path
                        group = PropertyNode::parse(prop, nullptr, ctx);
                    } else {
                        group = ExprNode::parse(groupItem, ctx);
                    }
                    addChild(_groupBy, group);
                }
            }

            if ( Value having = getCaseInsensitive(select, "HAVING") ) {
                setChild(_having, ExprNode::parse(having, ctx));
            }
            if ( Value limit = getCaseInsensitive(select, "LIMIT") ) {
                setChild(_limit, parseLimitOrOffset(limit, ctx, "LIMIT"));
            }
            if ( Value offset = getCaseInsensitive(select, "OFFSET") ) {
                setChild(_offset, parseLimitOrOffset(offset, ctx, "OFFSET"));
            }

        } else {
            // If not given a Dict or ["SELECT",...], assume it's a WHERE clause:
            addSource(new (ctx) SourceNode("_doc"), ctx);
            setChild(_where, ExprNode::parse(v, ctx));
        }

        if ( _what.empty() ) {
            // Default WHAT is id and sequence, for historical reasons:
            auto f = from();
            addChild(_what, new (ctx) WhatNode(new (ctx) MetaNode(MetaProperty::id, f)));
            addChild(_what, new (ctx) WhatNode(new (ctx) MetaNode(MetaProperty::sequence, f)));
        }

        Assert(!_sources.empty());
        Assert(ctx.from == from());
        Assert(!ctx.aliases.empty());

        // Check if this is an aggregate query, and whether it references a collections `deleted` property:
        _isAggregate = _distinct || !_groupBy.empty();
        visitTree([this](Node& node, unsigned /*depth*/) {
            if ( auto meta = dynamic_cast<MetaNode*>(&node) ) {
                // `meta()` calls that don't access any property implicity return the `deleted` property:
                auto prop = meta->property();
                if ( prop == MetaProperty::none || prop == MetaProperty::deleted ) meta->source()->setUsesDeleted();
            } else if ( auto fn = dynamic_cast<FunctionNode*>(&node) ) {
                // Look for aggregate functions:
                if ( fn->opFlags() & kOpAggregate ) _isAggregate = true;
            }
        });

        // Locate FTS and vector indexed expressions and add corresponding SourceNodes:
        addIndexes(ctx);

        for ( SourceNode* source : _sources ) {
            if ( !source->_usesDeleted && source->_collection.empty() && source->isCollection() ) {
                // The default collection may contain deleted documents in its main table,
                // so if the query didn't ask for deleted docs, add a condition to the WHERE
                // or ON clause that only passes live docs:
                auto       m    = new (ctx) MetaNode(MetaProperty::_notDeleted, source);
                ExprNode*& cond = source->isJoin() ? source->_joinOn : _where;
                if ( cond ) {
                    cond->setParent(nullptr);
                    auto a = new (ctx) OpNode(*lookupOp("AND", 2));
                    a->addArg(cond);
                    a->addArg(m);
                    cond = a;
                } else {
                    cond = m;
                }
                cond->setParent(source->isJoin() ? (Node*)source : (Node*)this);
            }
        }

        // Ensure sources' column names are unique
        for ( SourceNode* source : _sources ) source->disambiguateColumnName(ctx);

        // Ensure the WHAT nodes have non-empty, unique column names.
        // In the first pass, make sure explicitly named columns (WHAT nodes) have unique names;
        // in the second pass, the other columns will add "$n" or " #n" to make themselves unique.
        unordered_set<string> columnNames;
        for ( int x = 1; x >= 0; --x ) {
            for ( WhatNode* what : _what ) {
                if ( what->hasExplicitAlias() == bool(x) ) what->ensureUniqueColumnName(columnNames, ctx);
            }
        }
    }

    void SelectNode::registerAlias(AliasedNode* node, ParseContext& ctx) {
        slice alias    = node->alias();
        bool  inserted = ctx.aliases.insert({lowercase(string(alias)), node}).second;
        require(inserted, "duplicate alias '%.*s'", FMTSLICE(alias));
    }

    void SelectNode::addSource(SourceNode* source, ParseContext& ctx) {
        bool isFrom = false;
        if ( source->type() != SourceType::index ) {
            registerAlias(source, ctx);
            if ( source->isCollection() && !source->isJoin() ) {
                isFrom = true;
                require(_sources.empty(), "multiple non-join FROM items");
                ctx.from = source;
            }
            ctx.sources.emplace_back(source);
        }
        if ( !isFrom ) { require(!_sources.empty(), "first FROM item must be primary source"); }
        addChild(_sources, source);
    }

    void SelectNode::visitChildren(ChildVisitor const& visitor) {
        visitor(_sources)(_what)(_where)(_groupBy)(_having)(_orderBy)(_limit)(_offset);
    }

}  // namespace litecore::qt
