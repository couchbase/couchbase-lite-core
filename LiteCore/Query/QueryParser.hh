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
        QueryParser(const std::string& tableName, const std::string& bodyColumnName = "body")
        :_tableName(tableName)
        ,_bodyColumnName(bodyColumnName)
        { }

        void setBaseResultColumns(const std::vector<std::string>& c){_baseResultColumns = c;}

        void parse(const fleece::Value*);
        void parseJSON(slice);

        void parseJustExpression(const fleece::Value *expression);

        void writeCreateIndex(const std::string &name, const fleece::Array *expressions);

        static void writeSQLString(std::ostream &out, slice str);

        std::string SQL()  const                                    {return _sql.str();}

        const std::set<std::string>& parameters()                   {return _parameters;}
        const std::vector<std::string>& ftsTablesUsed() const       {return _ftsTables;}
        unsigned firstCustomResultColumn() const                    {return _1stCustomResultCol;}

        bool isAggregateQuery() const                               {return _isAggregateQuery;}

        static std::string expressionSQL(const fleece::Value*, const char *bodyColumnName = "body");
        std::string FTSTableName(const fleece::Value *key) const;
        std::string FTSTableName(const std::string &property) const;
        static std::string FTSColumnName(const fleece::Value *expression);

    private:
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
        void writeNotDeletedTest(unsigned tableIndex);

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
        void writePropertyGetter(slice fn, std::string property);
        void writeSQLString(slice str)              {writeSQLString(_sql, str);}
        void writeArgList(fleece::Array::iterator& operands);
        void writeColumnList(fleece::Array::iterator& operands);
        void writeResultColumn(const fleece::Value*);
        void writeCollation();
        void parseCollatableNode(const fleece::Value*);

        void parseJoin(const fleece::Dict*);

        unsigned findFTSProperties(const fleece::Value *node);
        size_t FTSPropertyIndex(const fleece::Value *matchLHS, bool canAdd =false);

        std::string _tableName;
        std::string _bodyColumnName;
        std::vector<std::string> _aliases;      // Aliased table/join names
        std::vector<std::string> _baseResultColumns;
        std::stringstream _sql;
        std::vector<const Operation*> _context;
        std::set<std::string> _parameters;
        std::set<std::string> _variables;
        std::vector<std::string> _ftsTables;
        unsigned _1stCustomResultCol {0};
        bool _aggregatesOK {false};
        bool _isAggregateQuery {false};
        static constexpr bool _includeDeleted {false};  // In future add an accessor to set this
        Collation _collation;
        bool _collationUsed {true};
    };

}
