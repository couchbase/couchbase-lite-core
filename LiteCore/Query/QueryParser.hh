//
//  QueryParser.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
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

        void writeCreateIndex(const fleece::Array *expressions);

        static void writeSQLString(std::ostream &out, slice str);

        std::string SQL()  const                                    {return _sql.str();}

        const std::vector<std::string>& ftsTablesUsed() const       {return _ftsTables;}
        unsigned firstCustomResultColumn() const                    {return _1stCustomResultCol;}

        bool isAggregateQuery() const                               {return _isAggregateQuery;}

        static std::string expressionSQL(const fleece::Value*, const char *bodyColumnName = "body");
        std::string indexName(const fleece::Array *keys) const;
        std::string FTSIndexName(const fleece::Value *key) const;
        std::string FTSIndexName(const std::string &property) const;

    private:
        struct Operation;
        static const Operation kOperationList[];
        static const Operation kOuterOperation, kArgListOperation, kColumnListOperation,
                               kExpressionListOperation;
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

        void parseFromClause(const fleece::Value *from);
        void writeFromClause(const fleece::Value *from);
        bool isValidJoinType(const std::string&);
        void writeOrderOrLimitClause(const fleece::Dict *operands,
                                     fleece::slice jsonKey,
                                     const char *keyword);

        void prefixOp(slice, fleece::Array::iterator&);
        void postfixOp(slice, fleece::Array::iterator&);
        void infixOp(slice, fleece::Array::iterator&);
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
        void writeStringLiteralAsProperty(slice str);

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
        Collation _collation;
    };

}
