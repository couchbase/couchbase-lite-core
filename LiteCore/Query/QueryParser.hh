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

        void parse(const fleece::Value *whereExpression, const fleece::Value *sortExpression);
        void parseJSON(slice whereJSON, slice sortJSON);

        std::string fromClause();
        std::string whereClause()               {return _sql.str();}
        std::string orderByClause()             {return _sortSQL.str();}

        const std::vector<std::string>& ftsProperties() const   {return _ftsProperties;}
        std::vector<std::string> ftsTableNames() const;

        static std::string propertyGetter(slice property, const char *bodyColumnName = "body");
        static void writeSQLString(std::ostream &out, slice str);

    private:
        struct Operation;
        static const Operation kOperationList[];
        static const Operation kOuterOperation, kArgListOperation, kOrderByOperation;
        struct JoinedOperations;
        static const JoinedOperations kJoinedOperationsList[];

        QueryParser(const QueryParser&) =delete;
        QueryParser& operator=(const QueryParser&) =delete;
        
        void parseNode(const fleece::Value*);
        void parseOpNode(const fleece::Array*);
        void handleOperation(const Operation*, slice actualOperator, fleece::Array::iterator& operands);

        void prefixOp(slice, fleece::Array::iterator&);
        void infixOp(slice, fleece::Array::iterator&);
        void betweenOp(slice, fleece::Array::iterator&);
        void existsOp(slice, fleece::Array::iterator&);
        void inOp(slice, fleece::Array::iterator&);
        void matchOp(slice, fleece::Array::iterator&);
        void parameterOp(slice, fleece::Array::iterator&);
        void propertyOp(slice, fleece::Array::iterator&);
        void selectOp(slice, fleece::Array::iterator&);
        void fallbackOp(slice, fleece::Array::iterator&);

        void countPropertyOp(slice, fleece::Array::iterator&);
        void existsPropertyOp(slice, fleece::Array::iterator&);

        bool writeNestedPropertyOpIfAny(const char *fnName, fleece::Array::iterator &operands);
        void writePropertyOp(const char *fnName, fleece::Array::iterator& operands);
        void writePropertyGetter(const char *fn, slice property);
        void writeSQLString(slice str)      {writeSQLString(_sql, str);}
        void writeArgList(fleece::Array::iterator& operands);

        void parseFTSMatch(slice property, const fleece::Value *match);
        size_t FTSPropertyIndex(const std::string &propertyPath);
        size_t AddFTSPropertyIndex(const std::string &propertyPath);
        void parseOrderBy(const fleece::Value*);
        void writeOrderBy(const fleece::Value*);
        void writeOrderByFTSRank(slice property);

        std::string _tableName;
        std::string _bodyColumnName;
        std::stringstream _sql;
        std::stringstream _sortSQL;
        std::string _propertyPath;
        std::vector<const Operation*> _context;
        std::set<std::string> _parameters;
        std::vector<std::string> _ftsProperties;
    };

}
