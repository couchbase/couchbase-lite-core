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

    void QueryTranslator::parse(FLValue v) {
        // Parse the query:
        unique_ptr<qt::QueryNode> query = make_unique<QueryNode>(v);

        // Set the SQLite table name for each SourceNode:
        for ( auto& source : query->sources() ) {
            if ( source->isUnnest() ) continue;
            string name = source->collection();
            if ( name.empty() ) name = _defaultCollectionName;
            if ( !source->scope().empty() ) name = source->scope() + "." + name;

            DeletionStatus delStatus = source->usesDeletedDocs() ? kLiveAndDeletedDocs : kLiveDocs;
            //FIXME: Support kDeletedDocs

            string tableName = _delegate.collectionTableName(name, delStatus);
            if ( name != _defaultCollectionName && !_delegate.tableExists(tableName) )
                fail("no such collection \"%s\"", name.c_str());

            if ( source->indexType() == IndexType::FTS ) {
                tableName = _delegate.FTSTableName(tableName, string(source->indexedExpressionJSON()));
                _ftsTables.push_back(tableName);
            } else if ( source->indexType() == IndexType::vector ) {
#ifdef COUCHBASE_ENTERPRISE
                auto vecSource = dynamic_cast<qt::VectorDistanceNode*>(source->indexedNodes().front().get());
                Assert(vecSource);
                tableName = _delegate.vectorTableName(tableName, string(vecSource->indexExpressionJSON()),
                                                      vecSource->metric());
#endif
            } else if ( source->isCollection() ) {
                if ( delStatus != kLiveAndDeletedDocs )  // that mode uses a fake union table
                    _kvTables.insert(tableName);
            }
            source->setTableName(tableName);
        }

        // Get the column titles:
        for ( auto& what : query->what() ) _columnTitles.push_back(what->columnName());

        // Get the parameter names:
        query->visitTree([this](Node& node, unsigned /*depth*/) {
            if ( auto p = dynamic_cast<ParameterNode*>(&node) ) {
                _parameters.emplace(p->name());
            } else if ( auto m = dynamic_cast<MetaNode*>(&node) ) {
                if ( m->property() == MetaProperty::expiration ) _usesExpiration = true;
            }
        });

        _isAggregateQuery   = query->isAggregate();
        _1stCustomResultCol = query->numPrependedColumns();

        // Finally, generate the SQL:
        _sql = writeSQL([&](SQLWriter& writer) { query->writeSQL(writer); });
    }

    void QueryTranslator::parseJSON(slice json) {
        Doc doc = Doc::fromJSON(json);
        parse(doc.root());
    }

    string QueryTranslator::writeSQL(function_ref<void(qt::SQLWriter&)> callback) {
        std::stringstream out;
        SQLWriter         writer(out);
        writer.bodyColumnName = _bodyColumnName;
        callback(writer);
        return out.str();
    }

    string QueryTranslator::expressionSQL(FLValue exprSource) {
        ParseContext ctx;
        auto         expr = ExprNode::parse(exprSource, ctx);
        return writeSQL([&](SQLWriter& writer) { writer << *expr; });
    }

#pragma mark - INDEX CREATION:

    void QueryTranslator::writeCreateIndex(const string& indexName, const string& onTableName,
                                           FLArrayIterator& whatExpressions, FLArray whereClause,
                                           bool isUnnestedTable) {
        _sql = writeSQL([&](SQLWriter& writer) {
            ParseContext ctx;

            writer << "CREATE INDEX " << sqlIdentifier(indexName) << " ON " << sqlIdentifier(onTableName) << " (";
            Array::iterator i(whatExpressions);
            if ( i.count() > 0 ) {
                delimiter comma(", ");
                for ( ; i; ++i ) {
                    unique_ptr<ExprNode> node;
                    if ( Value item = i.value(); item.asString() ) {
                        // If an index item is a string, wrap it in an array:
                        auto a = MutableArray::newArray();
                        a.append(item);
                        node = ExprNode::parse(a, ctx);
                    } else {
                        node = ExprNode::parse(i.value(), ctx);
                    }
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
                writer << " WHERE " << *where;
            }
        });
    }

    string QueryTranslator::whereClauseSQL(FLValue exprSource, string_view dbAlias) {
        if ( !exprSource ) return "";
        ParseContext ctx;
        auto         src = make_unique<SourceNode>(dbAlias);
        ctx.from         = src.get();
        auto expr        = ExprNode::parse(exprSource, ctx);
        ctx.clear();
        return writeSQL([&](SQLWriter& writer) { writer << "WHERE " << *expr; });
    }

    string QueryTranslator::functionCallSQL(slice fnName, FLValue arg, FLValue param) {
        ParseContext         ctx;
        auto                 argExpr = ExprNode::parse(arg, ctx);
        unique_ptr<ExprNode> paramExpr;
        if ( param ) paramExpr = ExprNode::parse(param, ctx);
        ctx.clear();
        return writeSQL([&](SQLWriter& writer) { writeFnGetter(fnName, *argExpr, paramExpr.get(), writer); });
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

    string QueryTranslator::eachExpressionSQL(FLValue) { error::_throw(error::Unimplemented); }

    string QueryTranslator::unnestedTableName(FLValue key) const { error::_throw(error::Unimplemented); }

    string QueryTranslator::predictiveTableName(FLValue) const { error::_throw(error::Unimplemented); }
}  // namespace litecore
