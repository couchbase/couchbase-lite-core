//
// QueryParser.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#pragma once
#include "Base.hh"
#include "UnicodeCollator.hh"
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fleece::impl {
    class Array;
    class ArrayIterator;
    class Dict;
    class Path;
    class Value;
}

namespace litecore {


    class QueryParser {
    public:
        /** Delegate knows about the naming & existence of tables. */
        class delegate {
        public:
            virtual ~delegate() =default;
            virtual std::string tableName() const =0;
            virtual std::string bodyColumnName() const        {return "body";}
            virtual std::string FTSTableName(const std::string &property) const =0;
            virtual std::string unnestedTableName(const std::string &property) const =0;
#ifdef COUCHBASE_ENTERPRISE
            virtual std::string predictiveTableName(const std::string &property) const =0;
#endif
            virtual bool tableExists(const std::string &tableName) const =0;
        };

        QueryParser(const delegate &delegate)
        :QueryParser(delegate, delegate.tableName(), delegate.bodyColumnName())
        { }

        void setTableName(const std::string &name)                  {_tableName = name;}
        void setBodyColumnName(const std::string &name)             {_bodyColumnName = name;}

        void parse(const fleece::impl::Value*);
        void parseJSON(slice);

        void parseJustExpression(const fleece::impl::Value *expression);

        void writeCreateIndex(const std::string &name,
                              fleece::impl::ArrayIterator &whatExpressions,
                              const fleece::impl::Array *whereClause,
                              bool isUnnestedTable);

        static void writeSQLString(std::ostream &out, slice str, char quote ='\'');

        std::string SQL()  const                                    {return _sql.str();}

        const std::set<std::string>& parameters()                   {return _parameters;}
        const std::vector<std::string>& ftsTablesUsed() const       {return _ftsTables;}
        unsigned firstCustomResultColumn() const                    {return _1stCustomResultCol;}
        const std::vector<std::string>& columnTitles() const        {return _columnTitles;}

        bool isAggregateQuery() const                               {return _isAggregateQuery;}
        bool usesExpiration() const                                 {return _checkedExpiration;}

        std::string expressionSQL(const fleece::impl::Value*);
        std::string whereClauseSQL(const fleece::impl::Value*, std::string_view dbAlias);
        std::string eachExpressionSQL(const fleece::impl::Value*);
        std::string FTSExpressionSQL(const fleece::impl::Value*);
        static std::string FTSColumnName(const fleece::impl::Value *expression);
        std::string unnestedTableName(const fleece::impl::Value *key) const;
        std::string predictiveIdentifier(const fleece::impl::Value *) const;
        std::string predictiveTableName(const fleece::impl::Value *) const;

    private:

        enum aliasType {
            kDBAlias,
            kJoinAlias,
            kResultAlias,
            kUnnestVirtualTableAlias,
            kUnnestTableAlias
        };

        QueryParser(const delegate &delegate, const std::string& tableName, const std::string& bodyColumnName)
        :_delegate(delegate)
        ,_tableName(tableName)
        ,_bodyColumnName(bodyColumnName)
        { }
        QueryParser(const QueryParser *qp)
        :QueryParser(qp->_delegate, qp->_tableName, qp->_bodyColumnName)
        { }


        struct Operation;
        static const Operation kOperationList[];
        static const Operation kOuterOperation, kArgListOperation, kColumnListOperation,
                               kResultListOperation, kExpressionListOperation,
                               kHighPrecedenceOperation;
        struct JoinedOperations;
        static const JoinedOperations kJoinedOperationsList[];

        QueryParser(const QueryParser &qp) =delete;
        QueryParser& operator=(const QueryParser&) =delete;

        void reset();
        void parseNode(const fleece::impl::Value*);
        void parseOpNode(const fleece::impl::Array*);
        void handleOperation(const Operation*, slice actualOperator, fleece::impl::ArrayIterator& operands);
        void parseStringLiteral(slice str);

        void writeSelect(const fleece::impl::Dict *dict);
        void writeSelect(const fleece::impl::Value *where, const fleece::impl::Dict *operands);
        unsigned writeSelectListClause(const fleece::impl::Dict *operands, slice key, const char *sql, bool aggregatesOK =false);

        void writeWhereClause(const fleece::impl::Value *where);
        void writeDeletionTest(const std::string &alias, bool isDeleted = false);

        void addAlias(const std::string &alias, aliasType);
        void parseFromClause(const fleece::impl::Value *from);
        void writeFromClause(const fleece::impl::Value *from);
        int parseJoinType(fleece::slice);
        bool writeOrderOrLimitClause(const fleece::impl::Dict *operands,
                                     fleece::slice jsonKey,
                                     const char *keyword);

        void prefixOp(slice, fleece::impl::ArrayIterator&);
        void postfixOp(slice, fleece::impl::ArrayIterator&);
        void infixOp(slice, fleece::impl::ArrayIterator&);
        void resultOp(slice, fleece::impl::ArrayIterator&);
        void arrayLiteralOp(slice, fleece::impl::ArrayIterator&);
        void betweenOp(slice, fleece::impl::ArrayIterator&);
        void existsOp(slice, fleece::impl::ArrayIterator&);
        void collateOp(slice, fleece::impl::ArrayIterator&);
        void concatOp(slice, fleece::impl::ArrayIterator&);
        void inOp(slice, fleece::impl::ArrayIterator&);
        void likeOp(slice, fleece::impl::ArrayIterator&);
        void matchOp(slice, fleece::impl::ArrayIterator&);
        void anyEveryOp(slice, fleece::impl::ArrayIterator&);
        void parameterOp(slice, fleece::impl::ArrayIterator&);
        void propertyOp(slice, fleece::impl::ArrayIterator&);
        void objectPropertyOp(slice, fleece::impl::ArrayIterator&);
        void blobOp(slice, fleece::impl::ArrayIterator&);
        void variableOp(slice, fleece::impl::ArrayIterator&);
        void missingOp(slice, fleece::impl::ArrayIterator&);
        void caseOp(slice, fleece::impl::ArrayIterator&);
        void selectOp(slice, fleece::impl::ArrayIterator&);
        void fallbackOp(slice, fleece::impl::ArrayIterator&);

        void functionOp(slice, fleece::impl::ArrayIterator&);

        void writeDictLiteral(const fleece::impl::Dict*);
        bool writeNestedPropertyOpIfAny(fleece::slice fnName, fleece::impl::ArrayIterator &operands);
        void writePropertyGetter(slice fn, fleece::impl::Path &&property,
                                 const fleece::impl::Value *param =nullptr);
        void writeFunctionGetter(slice fn, const fleece::impl::Value *source,
                                 const fleece::impl::Value *param =nullptr);
        void writeUnnestPropertyGetter(slice fn, fleece::impl::Path &property,
                                       const std::string &alias, aliasType);
        void writeEachExpression(fleece::impl::Path &&property);
        void writeEachExpression(const fleece::impl::Value *arrayExpr);
        void writeSQLString(slice str)              {writeSQLString(_sql, str);}
        void writeArgList(fleece::impl::ArrayIterator& operands);
        void writeColumnList(fleece::impl::ArrayIterator& operands);
        void writeResultColumn(const fleece::impl::Value*);
        void writeCollation();
        void parseCollatableNode(const fleece::impl::Value*);
        void writeMetaProperty(slice fn, const std::string &tablePrefix, const char *property);

        void parseJoin(const fleece::impl::Dict*);

        unsigned findFTSProperties(const fleece::impl::Value *root);
        void findPredictionCalls(const fleece::impl::Value *root);
        const std::string& indexJoinTableAlias(const std::string &key, const char *aliasPrefix =nullptr);
        const std::string&  FTSJoinTableAlias(const fleece::impl::Value *matchLHS, bool canAdd =false);
        const std::string&  predictiveJoinTableAlias(const fleece::impl::Value *expr, bool canAdd =false);
        std::string FTSTableName(const fleece::impl::Value *key) const;
        std::string expressionIdentifier(const fleece::impl::Array *expression, unsigned maxItems =0) const;
        void findPredictiveJoins(const fleece::impl::Value *node, std::vector<std::string> &joins);
        bool writeIndexedPrediction(const fleece::impl::Array *node);

        const delegate& _delegate;                  // delegate object (SQLiteKeyStore)
        std::string _tableName;                     // Name of the table containing documents
        std::string _bodyColumnName;                // Column holding doc bodies
        std::map<std::string, aliasType> _aliases;  // "AS..." aliases for db/joins/unnests
        std::string _dbAlias;                       // Alias of the db itself, "_doc" by default
        bool _propertiesUseSourcePrefix {false};    // Must properties include alias as prefix?
        std::vector<std::string> _columnTitles;     // Pretty names of result columns
        std::stringstream _sql;                     // The SQL being generated
        const fleece::impl::Value* _curNode;        // Current node being parsed
        std::vector<const Operation*> _context;     // Parser stack
        std::set<std::string> _parameters;          // Plug-in "$" parameters found in parsing
        std::set<std::string> _variables;           // Active variables, inside ANY/EVERY exprs
        std::map<std::string, std::string> _indexJoinTables;  // index table name --> alias
        std::vector<std::string> _ftsTables;        // FTS virtual tables being used
        unsigned _1stCustomResultCol {0};           // Index of 1st result after _baseResultColumns
        bool _aggregatesOK {false};                 // Are aggregate fns OK to call?
        bool _isAggregateQuery {false};             // Is this an aggregate query?
        bool _checkedDeleted {false};               // Has query accessed _deleted meta-property?
        bool _checkedExpiration {false};            // Has query accessed _expiration meta-property?
        Collation _collation;                       // Collation in use during parse
        bool _collationUsed {true};                 // Emitted SQL "COLLATION" yet?
        bool _functionWantsCollation {false};       // The current function wants to receive collation in its argument list
    };

}
