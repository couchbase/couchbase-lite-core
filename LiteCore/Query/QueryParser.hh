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
#include "Array.hh"
#include <memory>
#include <set>
#include <sstream>
#include <vector>

namespace fleece {
    class Value;
    class Array;
    class Dict;
    class Path;
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
            virtual bool tableExists(const std::string &tableName) const =0;
        };

        QueryParser(const delegate &delegate)
        :QueryParser(delegate, delegate.tableName(), delegate.bodyColumnName())
        { }

        void setTableName(const std::string &name)                  {_tableName = name;}
        void setBodyColumnName(const std::string &name)             {_bodyColumnName = name;}
        void setBaseResultColumns(const std::vector<std::string>& c){_baseResultColumns = c;}

        void parse(const fleece::Value*);
        void parseJSON(slice);

        void parseJustExpression(const fleece::Value *expression);

        void writeCreateIndex(const std::string &name,
                              fleece::Array::iterator &expressions,
                              bool isUnnestedTable);

        static void writeSQLString(std::ostream &out, slice str, char quote ='\'');

        std::string SQL()  const                                    {return _sql.str();}

        const std::set<std::string>& parameters()                   {return _parameters;}
        const std::vector<std::string>& ftsTablesUsed() const       {return _ftsTables;}
        unsigned firstCustomResultColumn() const                    {return _1stCustomResultCol;}

        bool isAggregateQuery() const                               {return _isAggregateQuery;}

        std::string expressionSQL(const fleece::Value*);
        std::string eachExpressionSQL(const fleece::Value*);
        static std::string FTSColumnName(const fleece::Value *expression);
        std::string unnestedTableName(const fleece::Value *key) const;

    private:

        enum aliasType {
            kDBAlias,
            kJoinAlias,
            kUnnestVirtualTableAlias,
            kUnnestTableAlias,
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
        void parseNode(const fleece::Value*);
        void parseOpNode(const fleece::Array*);
        void handleOperation(const Operation*, slice actualOperator, fleece::Array::iterator& operands);
        void parseStringLiteral(slice str);

        void writeSelect(const fleece::Dict *dict);
        void writeSelect(const fleece::Value *where, const fleece::Dict *operands);
        unsigned writeSelectListClause(const fleece::Dict *operands, slice key, const char *sql, bool aggregatesOK =false);

        void writeWhereClause(const fleece::Value *where);
        void writeNotDeletedTest(const std::string &alias);

        void parseFromClause(const fleece::Value *from);
        void writeFromClause(const fleece::Value *from);
        int parseJoinType(fleece::slice);
        void writeOrderOrLimitClause(const fleece::Dict *operands,
                                     fleece::slice jsonKey,
                                     const char *keyword);

        void prefixOp(slice, fleece::Array::iterator&);
        void postfixOp(slice, fleece::Array::iterator&);
        void infixOp(slice, fleece::Array::iterator&);
        void resultOp(slice, fleece::Array::iterator&);
        void arrayLiteralOp(slice, fleece::Array::iterator&);
        void betweenOp(slice, fleece::Array::iterator&);
        void existsOp(slice, fleece::Array::iterator&);
        void collateOp(slice, fleece::Array::iterator&);
        void inOp(slice, fleece::Array::iterator&);
        void matchOp(slice, fleece::Array::iterator&);
        void anyEveryOp(slice, fleece::Array::iterator&);
        void parameterOp(slice, fleece::Array::iterator&);
        void propertyOp(slice, fleece::Array::iterator&);
        void variableOp(slice, fleece::Array::iterator&);
        void missingOp(slice, fleece::Array::iterator&);
        void caseOp(slice, fleece::Array::iterator&);
        void selectOp(slice, fleece::Array::iterator&);
        void fallbackOp(slice, fleece::Array::iterator&);

        void functionOp(slice, fleece::Array::iterator&);

        bool writeNestedPropertyOpIfAny(fleece::slice fnName, fleece::Array::iterator &operands);
        void writePropertyGetter(slice fn, std::string property, const fleece::Value *param =nullptr);
        void writeUnnestPropertyGetter(slice fn, const std::string &property,
                                       const std::string &alias, aliasType);
        void writeEachExpression(const std::string &property);
        void writeEachExpression(const fleece::Value *arrayExpr);
        void writeSQLString(slice str)              {writeSQLString(_sql, str);}
        void writeArgList(fleece::Array::iterator& operands);
        void writeColumnList(fleece::Array::iterator& operands);
        void writeResultColumn(const fleece::Value*);
        void writeCollation();
        void parseCollatableNode(const fleece::Value*);

        void parseJoin(const fleece::Dict*);

        unsigned findFTSProperties(const fleece::Value *node);
        size_t FTSPropertyIndex(const fleece::Value *matchLHS, bool canAdd =false);
        std::string FTSTableName(const fleece::Value *key) const;

        const delegate& _delegate;                  // delegate object (SQLiteKeyStore)
        std::string _tableName;                     // Name of the table containing documents
        std::string _bodyColumnName;                // Column holding doc bodies
        std::map<std::string, aliasType> _aliases;  // "AS..." aliases for db/joins/unnests
        std::string _dbAlias;                       // Alias of the db itself, "_doc" by default
        bool _propertiesUseAliases {false};         // Must properties include alias as prefix?
        std::vector<std::string> _baseResultColumns;// Default columns to always emit
        std::stringstream _sql;                     // The SQL being generated
        std::vector<const Operation*> _context;     // Parser stack
        std::set<std::string> _parameters;          // Plug-in "$" parameters found in parsing
        std::set<std::string> _variables;           // Active variables, inside ANY/EVERY exprs
        std::vector<std::string> _ftsTables;        // FTS virtual tables being used
        unsigned _1stCustomResultCol {0};           // Index of 1st result after _baseResultColumns
        bool _aggregatesOK {false};                 // Are aggregate fns OK to call?
        bool _isAggregateQuery {false};             // Is this an aggregate query?
        static constexpr bool _includeDeleted {false};  // In future add an accessor to set this
        Collation _collation;                       // Collation in use during parse
        bool _collationUsed {true};                 // Emitted SQL "COLLATION" yet?
    };

}
