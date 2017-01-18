//
//  QueryParser.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
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
        void setDefaultOffset(const std::string &o)                 {_defaultOffset = o;}
        void setDefaultLimit(const std::string &l)                  {_defaultLimit = l;}

        void parse(const fleece::Value*);
        void parseJSON(slice);

        void parseJustExpression(const fleece::Value *expression);

        std::string SQL()                                           {return _sql.str();}

        const std::vector<std::string>& ftsProperties() const       {return _ftsProperties;}
        unsigned firstCustomResultColumn() const                    {return _1stCustomResultCol;}

        static std::string propertyGetter(slice property, const char *bodyColumnName = "body");
        static void writeSQLString(std::ostream &out, slice str);

    private:
        struct Operation;
        static const Operation kOperationList[];
        static const Operation kOuterOperation, kArgListOperation, kOrderByOperation;
        struct JoinedOperations;
        static const JoinedOperations kJoinedOperationsList[];

        QueryParser(const QueryParser &qp) =delete;
        QueryParser& operator=(const QueryParser&) =delete;

        void reset();
        void parseNode(const fleece::Value*);
        void parseOpNode(const fleece::Array*);
        void handleOperation(const Operation*, slice actualOperator, fleece::Array::iterator& operands);

        void writeSelect(const fleece::Dict *dict);
        void writeSelect(const fleece::Value *where, const fleece::Dict *operands);

        void prefixOp(slice, fleece::Array::iterator&);
        void postfixOp(slice, fleece::Array::iterator&);
        void infixOp(slice, fleece::Array::iterator&);
        void betweenOp(slice, fleece::Array::iterator&);
        void existsOp(slice, fleece::Array::iterator&);
        void inOp(slice, fleece::Array::iterator&);
        void matchOp(slice, fleece::Array::iterator&);
        void anyEveryOp(slice, fleece::Array::iterator&);
        void parameterOp(slice, fleece::Array::iterator&);
        void propertyOp(slice, fleece::Array::iterator&);
        void variableOp(slice, fleece::Array::iterator&);
        void missingOp(slice, fleece::Array::iterator&);
        void selectOp(slice, fleece::Array::iterator&);
        void fallbackOp(slice, fleece::Array::iterator&);

        void functionOp(slice, fleece::Array::iterator&);

        bool writeNestedPropertyOpIfAny(const char *fnName, fleece::Array::iterator &operands);
        void writePropertyGetter(const std::string &fn, const std::string &property);
        void writeSQLString(slice str)              {writeSQLString(_sql, str);}
        void writeArgList(fleece::Array::iterator& operands);
        void writeResultColumn(const fleece::Value*);

        void findFTSProperties(const fleece::Value *node);
        size_t FTSPropertyIndex(const std::string &propertyPath);
        size_t AddFTSPropertyIndex(const std::string &propertyPath);

        std::string _tableName;
        std::string _bodyColumnName;
        std::vector<std::string> _baseResultColumns;
        std::string _defaultOffset, _defaultLimit;
        std::stringstream _sql;
        std::string _propertyPath;
        std::vector<const Operation*> _context;
        std::set<std::string> _parameters;
        std::set<std::string> _variables;
        std::vector<std::string> _ftsProperties;
        unsigned _1stCustomResultCol {0};
    };

}
