//
// NodesToSQL.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SQLWriter.hh"
#include "ExprNodes.hh"
#include "IndexedNodes.hh"
#include "SelectNodes.hh"
#include "Delimiter.hh"
#include "Error.hh"
#include "SQLUtil.hh"
#include "StringUtil.hh"
#include "TranslatorTables.hh"
#include "TranslatorUtils.hh"
#include <iostream>
#include <type_traits>

namespace litecore::qt {
    using namespace fleece;
    using namespace std;

    void Node::writeSQL(std::ostream& out) const {
        SQLWriter ctx{out};
        writeSQL(ctx);
    }

    string Node::SQLString() const {
        std::stringstream s;
        writeSQL(s);
        return s.str();
    }

    void RawSQLNode::writeSQL(SQLWriter& ctx) const { ctx << _sql; }

    void LiteralNode::writeSQL(SQLWriter& ctx) const {
        switch ( _literal.type() ) {
            case kFLNull:
                ctx << kNullFnName << "()";
                break;
            case kFLNumber:
                ctx << _literal.toString();
                break;
            case kFLBoolean:
                ctx << kBoolFnName << '(' << (int)_literal.asBool() << ')';
                break;
            case kFLString:
                ctx << sqlString(_literal.asString());
                break;
            default:
                fail("internal error: invalid LiteralNode");
        }
    }

    void MetaNode::writeSQL(SQLWriter& ctx) const {
        string aliasDot;
        if ( _source ) aliasDot = CONCAT(sqlIdentifier(_source->alias()) << ".");
        writeMetaSQL(aliasDot, _property, ctx);
    }

    void MetaNode::writeMetaSQL(string_view aliasDot, MetaProperty meta, SQLWriter& ctx) {
        switch ( meta ) {
            case MetaProperty::none:
                {
                    ctx << kDictOfFunctionSpec.sqlite_name << '(';
                    delimiter comma(", ");
                    for ( int i = 0; i < kNumMetaProperties; i++ ) {
                        meta = MetaProperty(i + 1);
                        if ( meta != MetaProperty::rowid ) {  // rowid is not official
                            ctx << comma << sqlString(kMetaPropertyNames[i]) << ", ";
                            writeMetaSQL(aliasDot, meta, ctx);
                        }
                    }
                    ctx << ')';
                    break;
                }
            case MetaProperty::id:
            case MetaProperty::sequence:
            case MetaProperty::expiration:
            case MetaProperty::rowid:
                ctx << aliasDot << kMetaSQLiteNames[int(meta) - 1];
                break;
            case MetaProperty::deleted:
                ctx << '(' << aliasDot << "flags & 1 != 0)";
                break;
            case MetaProperty::_notDeleted:
                ctx << '(' << aliasDot << "flags & 1 = 0)";
                break;
            case MetaProperty::revisionID:
                ctx << kVersionFnName << '(' << aliasDot << "version)";
                break;
        }
    }

    void PropertyNode::writeSQL(SQLWriter& ctx, slice sqliteFnName, ExprNode* param) const {
        if ( !sqliteFnName ) sqliteFnName = _sqliteFn;
        if ( _result ) {
            // Property is a result alias, or a subpath thereof:
            if ( sqliteFnName.empty() ) {
                ctx << sqlIdentifier(_result->alias());
            } else {
                ctx << sqliteFnName << '(' << sqlIdentifier(_result->alias()) << ", " << sqlString(_path.toString())
                    << ')';
            }
        } else {
            string aliasDot = "";
            if ( _source && !_source->alias().empty() ) aliasDot = CONCAT(sqlIdentifier(_source->alias()) << ".");
            if ( _source && _source->isUnnest() && _source->tableName().empty() && _path.count() == 0 ) {
                // Accessing the outer item of a `fl_each` table-valued function:
                ctx << aliasDot << "value";
            } else {
                // Regular property access, using the sqliteFnName as accessor:
                bool extraCloseParen = false;
                if (_source && _source->isUnnest() && !_source->tableName().empty()) {
                    if (sqliteFnName == kRootFnName || sqliteFnName == kNestedValueFnName)
                        sqliteFnName = kUnnestedValueFnName;    // Use `fl_unnested_value` to access unnest index table
                    else if (sqliteFnName == kResultFnName) {
                        sqliteFnName = kUnnestedValueFnName;
                        ctx << kResultFnName << '(';
                        extraCloseParen = true;
                    }
                }
                ctx << sqliteFnName << '(' << aliasDot << ctx.bodyColumnName;
                if ( _path.count() > 0 ) ctx << ", " << sqlString(_path.toString());
                if ( param ) ctx << ", " << *param;
                ctx << ")";
                if (extraCloseParen)
                    ctx << ')';
            }
        }
    }

    void ParameterNode::writeSQL(SQLWriter& ctx) const { ctx << '$' << sqlIdentifier("_" + _name); }

    void VariableNode::writeSQL(SQLWriter& ctx) const {
        ctx << sqlIdentifier("_" + _name);
        if ( _returnBody ) ctx << '.' << ctx.bodyColumnName;
        else
            ctx << ".value";
    }

    void OpNode::writeSQL(SQLWriter& ctx) const {
        Parenthesize p(ctx, _op.precedence);

        switch ( _op.type ) {
            case OpType::missing:
                ctx << "NULL";
                break;
            case OpType::prefix:
            case OpType::exists:
                Assert(_operands.size() == 1);
                ctx << _op.name;
                if ( isalpha(_op.name[_op.name.size - 1]) ) ctx << ' ';  // No space after `-`
                ctx << _operands[0];
                break;
            case OpType::infix:
                {
                    string    spaced = " " + string(_op.name) + " ";
                    delimiter delim(spaced.c_str());
                    for ( auto& operand : _operands ) ctx << delim << operand;
                    break;
                }
            case OpType::postfix:
                Assert(_operands.size() == 1);
                ctx << _operands[0] << ' ' << _op.name;
                break;
            case OpType::is:
            case OpType::is_not:
                {
                    slice opName = _op.name;
                    if ( auto lit = dynamic_cast<LiteralNode*>(_operands[1].get()) ) {
                        // Ugly special case where SQLite's semantics for 'IS [NOT]' don't match N1QL's (#410)
                        if ( lit->literal().type() == kFLNull ) opName = (_op.type == OpType::is) ? "=" : "!=";
                    }
                    ctx << _operands[0] << ' ' << opName << ' ' << _operands[1];
                    break;
                }
            case OpType::between:
                Assert(_operands.size() == 3);
                ctx << _operands[0] << " BETWEEN " << _operands[1] << " AND " << _operands[2];
                break;
            case OpType::in:
            case OpType::not_in:
                {
                    ctx << _operands[0] << ' ' << _op.name << " (";
                    WithPrecedence wp(ctx, kArgListPrecedence);
                    delimiter      comma(", ");
                    for ( size_t i = 1; i < _operands.size(); i++ ) ctx << comma << _operands[i];
                    ctx << ')';
                    break;
                }
            case OpType::like:
                {
                    // If the LHS has a COLLATE spec, emit a custom function because SQLite's
                    // built-in LIKE is case-sensitive.
                    auto lhs = _operands[0].get();
                    auto rhs = _operands[1].get();
                    if ( auto coll = dynamic_cast<CollateNode*>(lhs); coll && !coll->isBinary() ) {
                        lhs = coll->child();
                        ctx << kLikeFnName << '(' << *lhs << ", " << *rhs << ", "
                            << sqlString(coll->collation().sqliteName()) << ')';
                    } else {
                        assert(lhs);
                        ctx << *lhs << ' ' << _op.name << ' ' << *rhs << " ESCAPE '\\'";
                    }
                    break;
                }
            case OpType::objectProperty:
                {
                    WithPrecedence wp(ctx, kArgListPrecedence);
                    ctx << kNestedValueFnName << '(' << _operands[0] << ", " << _operands[1] << ')';
                    break;
                }
            case OpType::Case:
                {
                    // Check whether the test expression is a literal `null`:
                    ctx << "CASE";
                    auto test = _operands[0].get();
                    assert(test);
                    if ( auto literal = dynamic_cast<LiteralNode*>(test);
                         literal && literal->literal().type() == kFLNull ) {
                        test = nullptr;
                    } else {
                        ctx << ' ' << *test;
                    }
                    size_t i;
                    for ( i = 1; i < _operands.size() - 1; i += 2 ) {
                        ctx << " WHEN " << _operands[i] << " THEN " << _operands[i + 1];
                    }
                    ctx << " ELSE ";
                    if ( i < _operands.size() ) ctx << _operands[i];
                    else
                        ctx << kNullFnName << "()";
                    ctx << " END";
                    break;
                }
            case OpType::blob:
                if ( auto prop = dynamic_cast<PropertyNode*>(_operands[0].get()) )
                    prop->writeSQL(ctx, kBlobFnName, nullptr);
                else
                    fail("argument of BLOB() must be a document property");
                break;
            default:
                fail("internal error: Operation type %d not handled in writeSQL", int(_op.type));
        }
    }

    void FunctionNode::writeSQL(SQLWriter& ctx) const {
        delimiter comma(", ");
        ctx << (_fn.sqlite_name ? _fn.sqlite_name : _fn.name) << '(';
        WithPrecedence wp(ctx, kArgListPrecedence);
        for ( auto& arg : _args ) ctx << comma << arg;
        ctx << ')';
    }

    void CollateNode::writeSQL(SQLWriter& ctx) const {
        Parenthesize p(ctx, kCollatePrecedence);
        ctx << _child << " COLLATE " << sqlIdentifier(_collation.sqliteName());
    }

    void AnyEveryNode::writeSQL(SQLWriter& ctx) const {
        ExprNode& collection     = this->collection();
        auto      collectionProp = dynamic_cast<PropertyNode*>(&collection);
        ExprNode& predicate      = this->predicate();

        if ( _op.type == OpType::any ) {
            if ( auto e = dynamic_cast<OpNode*>(&predicate); e && e->op().name == "=" ) {
                if ( dynamic_cast<VariableNode*>(e->operand(0)) ) {
                    // If predicate is `var = value`, generate `fl_contains(array, value)` instead
                    writeFnGetter(kContainsFnName, collection, e->operand(1), ctx);
                    return;
                }
            }
        }

        if ( _op.type == OpType::anyAndEvery ) {
            ctx << '(';
            writeFnGetter(kCountFnName, collection, nullptr, ctx);
            ctx << " > 0 AND ";
        }

        if ( _op.type != OpType::any ) ctx << "NOT ";
        ctx << "EXISTS (SELECT 1 FROM ";
        if ( collectionProp ) {
            collectionProp->setSQLiteFn(kEachFnName);
            ctx << collection;
        } else {
            WithPrecedence listP(ctx, kArgListPrecedence);
            ctx << kEachFnName << '(' << collection << ')';
        }
        ctx << " AS " << sqlIdentifier("_" + string(_variableName)) << " WHERE ";
        if ( _op.type != OpType::any ) ctx << "NOT (";
        ctx << predicate;
        if ( _op.type != OpType::any ) ctx << ')';
        ctx << ')';

        if ( _op.type == OpType::anyAndEvery ) ctx << ')';
    }

    void WhatNode::writeSQL(SQLWriter& ctx) const {
        if ( auto flags = _expr->opFlags(); (flags & (kOpNumberResult | kOpStringResult)) != 0 ) {
            ctx << _expr;
        } else {
            ctx << ((flags & kOpBoolResult) ? kBoolResultFnName : kResultFnName);
            WithPrecedence listP(ctx, kArgListPrecedence);
            ctx << '(' << _expr << ')';
        }
        if ( _hasExplicitAlias ) ctx << " AS " << sqlIdentifier(_alias);
    }

    void SourceNode::writeSQL(SQLWriter& ctx) const {
        if ( _unnest ) {
            ctx << "JOIN ";
            if (_tableName.empty()) {
                // Unindexed UNNEST, using `fl_each`:
                if ( auto prop = dynamic_cast<PropertyNode*>(_unnest.get()) ) {
                    prop->setSQLiteFn(kEachFnName);
                    ctx << *prop;
                } else {
                    WithPrecedence listP(ctx, kArgListPrecedence);
                    ctx << kEachFnName << '(' << _unnest << ')';
                }
            } else {
                // Indexed UNNEST:
                auto sourceTable = _unnest->source();
                Assert(sourceTable);
                ctx << sqlIdentifier(_tableName) << " AS " << sqlIdentifier(_alias);
                ctx << " ON " << sqlIdentifier(_alias) << ".docid=" << sqlIdentifier(sourceTable->alias()) << ".rowid";
                return;
            }
        } else {
            if ( _join > JoinType::none ) {
                ctx << kJoinTypeNames[int(_join)] << " JOIN ";
            } else {
                ctx << "FROM ";
            }
            Assert(!_tableName.empty(), "QueryTranslator client didn't set Source's tableName");
            if ( isIndex() ) _indexedNodes[0]->writeSourceTable(ctx, _tableName);
            else
                ctx << sqlIdentifier(_tableName);
        }
        if ( !_alias.empty() ) { ctx << " AS " << sqlIdentifier(_alias); }
        if ( _joinOn ) { ctx << " ON " << _joinOn; }
    }

    void SelectNode::writeSQL(SQLWriter& ctx) const {
        Parenthesize p(ctx, kSelectPrecedence);

        ctx << "SELECT ";
        if ( _distinct ) ctx << "DISTINCT ";

        {
            delimiter comma(", ");
            // Write extra columns used for FTS
            writeFTSColumns(ctx, comma);
            // ...before the actual columns:
            for ( auto& what : _what ) ctx << comma << what;
        }

        ctx << ' ' << *_from;
        for ( auto& join : _sources )
            if ( join->isJoin() || join->isUnnest() ) ctx << ' ' << join;

        if ( _where ) ctx << " WHERE " << _where;

        if ( !_groupBy.empty() ) {
            ctx << " GROUP BY ";
            delimiter comma(", ");
            for ( auto& g : _groupBy ) ctx << comma << g;
        }

        if ( _having ) ctx << " HAVING " << _having;

        if ( !_orderBy.empty() ) {
            ctx << " ORDER BY ";
            delimiter comma(", ");
            for ( size_t i = 0; i < _orderBy.size(); ++i ) {
                ctx << comma << _orderBy[i];
                if ( _orderDesc[i] ) ctx << " DESC";
            }
        }

        if ( _limit ) ctx << " LIMIT " << _limit;
        else if ( _offset )
            ctx << " LIMIT -1";  // SQLite does not allow OFFSET without a LIMIT first
        if ( _offset ) ctx << " OFFSET " << _offset;
    }
}  // namespace litecore::qt
