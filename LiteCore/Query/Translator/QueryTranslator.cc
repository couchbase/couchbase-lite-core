//
// QueryTranslator.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "QueryTranslator.hh"
#include "Delimiter.hh"
#include "Error.hh"
#include "ExprNodes.hh"
#include "IndexedNodes.hh"
#include "Logging.hh"
#include "SelectNodes.hh"
#include "SQLWriter.hh"
#include "TranslatorTables.hh"
#include "TranslatorUtils.hh"
#include <sstream>

namespace litecore {
    using namespace std;
    using namespace fleece;
    using namespace litecore::qt;

    QueryTranslator::QueryTranslator(const Delegate& delegate, string defaultCollectionName, string defaultTableName)
        : _delegate(delegate)
        , _defaultCollectionName(std::move(defaultCollectionName))
        , _defaultTableName(std::move(defaultTableName))
        , _bodyColumnName("body") {}

    QueryTranslator::~QueryTranslator() = default;

    RootContext QueryTranslator::makeRootContext() const {
        RootContext root;
#ifdef COUCHBASE_ENTERPRISE
        root.hasPredictiveIndex = [&](string_view id) -> bool {
            string indexTable = _delegate.predictiveTableName(_defaultTableName, string(id));
            return _delegate.tableExists(indexTable);
        };
#endif
        return root;
    }

    void QueryTranslator::parse(FLValue v) {
        RootContext ctx = makeRootContext();

        // Parse the query into a Node tree:
        SelectNode* query = new (ctx) SelectNode(v, ctx);
        query->postprocess(ctx);

        query->visitTree([&](Node& node, unsigned /*depth*/) {
            if ( auto source = dynamic_cast<SourceNode*>(&node) ) {
                // Set the SQLite table name for each SourceNode:
                assignTableNameToSource(source, ctx);
            } else if ( auto p = dynamic_cast<ParameterNode*>(&node) ) {
                // Capture the parameter names:
                _parameters.emplace(p->name());
            } else if ( !_usesExpiration ) {
                // Detect whether the query uses the `expiration` column:
                if ( auto m = dynamic_cast<MetaNode*>(&node); m && m->property() == MetaProperty::expiration )
                    _usesExpiration = true;
            }
        });

        // Get the column titles:
        for ( WhatNode* what : query->what() ) _columnTitles.emplace_back(what->columnName());

        _isAggregateQuery   = query->isAggregate();
        _1stCustomResultCol = query->numPrependedColumns();

        // Finally, generate the SQL:
        _sql = writeSQL([&](SQLWriter& writer) { query->writeSQL(writer); });
    }

    void QueryTranslator::parseJSON(slice json) {
        Doc doc = Doc::fromJSON(json);
        parse(doc.root());
    }

    string QueryTranslator::writeSQL(function_ref<void(SQLWriter&)> callback) {
        std::stringstream out;
        SQLWriter         writer(out);
        writer.bodyColumnName = _bodyColumnName;
        callback(writer);
        return out.str();
    }

    string QueryTranslator::expressionSQL(FLValue exprSource) {
        RootContext ctx  = makeRootContext();
        auto        expr = ExprNode::parse(exprSource, ctx);
        expr->postprocess(ctx);

        // Set the SQLite table name for each SourceNode:
        expr->visitTree([&](Node& node, unsigned /*depth*/) {
            if ( auto source = dynamic_cast<SourceNode*>(&node) ) assignTableNameToSource(source, ctx);
        });

        return writeSQL([&](SQLWriter& writer) { writer << *expr; });
    }

    void QueryTranslator::assignTableNameToSource(SourceNode* source, ParseContext& ctx) {
        if ( source->tableName().empty() ) {
            string tableName = tableNameForSource(source, ctx);
            if ( !tableName.empty() && (tableName == _defaultTableName || _delegate.tableExists(tableName)) )
                source->setTableName(ctx.newString(tableName));
        }
    }

    string QueryTranslator::tableNameForSource(SourceNode* source, ParseContext& ctx) {
        string tableName(source->tableName());
        if ( !tableName.empty() ) return tableName;

        if ( auto unnest = dynamic_cast<UnnestSourceNode*>(source) ) {
            // Check whether there's an array index we can use for an UNNEST:
            auto unnestSrc = unnest->unnestExpression()->source();
            if ( !unnestSrc ) return "";
            tableName = _delegate.unnestedTableName(tableNameForSource(unnestSrc, ctx), unnest->unnestIdentifier());
            if ( _delegate.tableExists(tableName) ) source->setTableName(ctx.newString(tableName));
        } else {
            string name(source->collection());
            if ( name.empty() ) name = _defaultCollectionName;
            if ( !source->scope().empty() ) name = string(source->scope()) + "." + name;

            DeletionStatus delStatus = source->usesDeletedDocs() ? kLiveAndDeletedDocs : kLiveDocs;
            //FIXME: Support kDeletedDocs

            tableName = _delegate.collectionTableName(name, delStatus);
            if ( name != _defaultCollectionName && !_delegate.tableExists(tableName) )
                fail("no such collection \"%s\"", name.c_str());

            if ( auto index = dynamic_cast<IndexSourceNode*>(source) ) {
                switch ( index->indexType() ) {
                    case IndexType::FTS:
                        tableName = _delegate.FTSTableName(tableName, string(index->indexID()));
                        _ftsTables.push_back(tableName);
                        break;
#ifdef COUCHBASE_ENTERPRISE
                    case IndexType::vector:
                        {
                            auto vecSource = dynamic_cast<VectorDistanceNode*>(index->indexedNode());
                            Assert(vecSource);
                            tableName = _delegate.vectorTableName(tableName, string(vecSource->indexID()),
                                                                  vecSource->metric());
                            break;
                        }
                    case IndexType::prediction:
                        {
                            auto predSource = index->indexedNode();
                            tableName       = _delegate.predictiveTableName(tableName, string(predSource->indexID()));
                            break;
                        }
#endif
                }
            } else if ( source->isCollection() ) {
                if ( delStatus != kLiveAndDeletedDocs )  // that mode uses a fake union table
                    _kvTables.insert(tableName);
            }
        }
        return tableName;
    }

#pragma mark - INDEX CREATION:

    void QueryTranslator::writeCreateIndex(const string& indexName, const string& onTableName,
                                           FLArrayIterator& whatExpressions, FLArray whereClause,
                                           bool isUnnestedTable) {
        _sql = writeSQL([&](SQLWriter& writer) {
            RootContext ctx = makeRootContext();

            SourceNode* source;
            if ( isUnnestedTable ) {
                source   = new (ctx) UnnestSourceNode();
                ctx.from = source;
            }

            writer << "CREATE INDEX " << sqlIdentifier(indexName) << " ON " << sqlIdentifier(onTableName) << " (";
            Array::iterator i(whatExpressions);
            if ( i.count() > 0 ) {
                delimiter comma(", ");
                for ( ; i; ++i ) {
                    ExprNode* node;
                    if ( Value item = i.value(); item.asString() ) {
                        // If an index item is a string, wrap it in an array:
                        auto a = MutableArray::newArray();
                        a.append(item);
                        node = ExprNode::parse(a, ctx);
                    } else {
                        node = ExprNode::parse(i.value(), ctx);
                    }
                    node->postprocess(ctx);
                    writer << comma << *node;
                }
            } else {
                // No expressions; index the entire body (this is used with unnested/array tables):
                Assert(isUnnestedTable);
                writer << kUnnestedValueFnName << "(" << _bodyColumnName << ")";
            }
            writer << ')';
            if ( whereClause && !isUnnestedTable ) {
                auto where = ExprNode::parse(Array(whereClause), ctx);
                where->postprocess(ctx);
                writer << " WHERE " << *where;
            }
        });
    }

    string QueryTranslator::whereClauseSQL(FLValue exprSource, string_view dbAlias) {
        if ( !exprSource ) return "";
        RootContext ctx = makeRootContext();
        auto        src = new (ctx) SourceNode(ctx.newString(dbAlias));
        ctx.from        = src;
        auto expr       = ExprNode::parse(exprSource, ctx);
        expr->postprocess(ctx);
        return writeSQL([&](SQLWriter& writer) { writer << "WHERE " << *expr; });
    }

    string QueryTranslator::functionCallSQL(slice fnName, FLValue arg, FLValue param) {
        RootContext ctx     = makeRootContext();
        auto        argExpr = ExprNode::parse(arg, ctx);
        argExpr->postprocess(ctx);
        ExprNode* paramExpr = nullptr;
        if ( param ) {
            paramExpr = ExprNode::parse(param, ctx);
            paramExpr->postprocess(ctx);
        }
        return writeSQL([&](SQLWriter& writer) { writeFnGetter(fnName, *argExpr, paramExpr, writer); });
    }

    string QueryTranslator::FTSExpressionSQL(FLValue exprFleece) {
        return functionCallSQL(kFTSValueFnName, exprFleece);
    }

    string QueryTranslator::FTSColumnName(FLValue expression) {
        Array           arr = requiredArray(Value(expression), "FTS index expression");
        Array::iterator iter(arr);
        slice           op = requiredString(*iter, "first item of FTS index expression");
        ++iter;
        KeyPath path = parsePath(op, &iter);
        require(path.count() > 0, "invalid property expression");
        return string(path);
    }

    string QueryTranslator::vectorToIndexExpressionSQL(FLValue exprToIndex, unsigned dimensions) {
        auto a = MutableArray::newArray();
        a.append(dimensions);
        Value dimAsFleece = a[0];
        return functionCallSQL(kVectorToIndexFnName, exprToIndex, dimAsFleece);
    }

    string QueryTranslator::unnestedTableName(FLValue flExpr) const {
        RootContext ctx  = makeRootContext();
        auto        expr = ExprNode::parse(flExpr, ctx);
        expr->postprocess(ctx);

        string propertyStr;
        if ( auto prop = dynamic_cast<PropertyNode*>(expr) ) {
            propertyStr = string(prop->path());
        } else {
            propertyStr = expressionIdentifier(Value(flExpr).asArray());
        }
        return _delegate.unnestedTableName(_defaultTableName, propertyStr);
    }

    string QueryTranslator::eachExpressionSQL(FLValue flExpr) {
        RootContext ctx  = makeRootContext();
        auto        expr = ExprNode::parse(flExpr, ctx);

        auto prop = dynamic_cast<PropertyNode*>(expr);
        Assert(prop, "eachExpressionSQL: expression must be a property path");
        prop->setSQLiteFn(kEachFnName);
        return writeSQL([&prop](SQLWriter& sql) { prop->writeSQL(sql); });
    }

    string QueryTranslator::predictiveIdentifier(FLValue expression) const {
        auto array = Value(expression).asArray();
        if ( array.count() < 2 || !array[0].asString().caseEquivalent("PREDICTION()") )
            fail("Invalid PREDICTION() call");
        return expressionIdentifier(array, 3);  // ignore the output-property parameter
    }

#ifdef COUCHBASE_ENTERPRISE
    string QueryTranslator::predictiveTableName(FLValue expression) const {
        return _delegate.predictiveTableName(_defaultTableName, predictiveIdentifier(expression));
    }
#endif
}  // namespace litecore
