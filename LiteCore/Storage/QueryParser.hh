//
//  QueryParser.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include <memory>
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
        QueryParser(const std::string& tableName, const std::string& jsonColumnName = "body")
        :_tableName(tableName)
        ,_jsonColumnName(jsonColumnName)
        { }

        void parse(const fleece::Value *whereExpression, const fleece::Value *sortExpression);
        void parseJSON(slice whereJSON, slice sortJSON);

        std::string fromClause();
        std::string whereClause()               {return _sql.str();}
        std::string orderByClause()             {return _sortSQL.str();}

        std::vector<std::string> ftsTableNames() const;

        static std::string propertyGetter(slice property, const char *sqlColumn = "body");
        static void writeSQLString(std::ostream &out, slice str);

    private:
        void writePropertyPathString(slice property);
        void writePropertyGetter(const char *fn, slice property);
        void writeSQLString(slice str)      {writeSQLString(_sql, str);}

        void parsePredicate(const fleece::Value*);
        void parseTerm(slice key, const fleece::Value*);
        void parseSubPropertyTerm(slice key, const fleece::Dict*);
        void parseElemMatch(slice property, const fleece::Value*);
        void parseElemMatchTerm(const std::string &key, const fleece::Value*);
        void parseFTSMatch(slice property, const fleece::Value *match);
        void writeBooleanExpr(const fleece::Value *terms, const char *op);
        void writeLiteral(const fleece::Value *literal);
        void writePropertyGetterLeftOpen(const char *fn, slice property);
        void parseSort(const fleece::Value*);
        void writeOrderBy(const fleece::Value*);

        std::string _tableName;
        std::string _jsonColumnName;
        std::stringstream _sql;
        std::stringstream _sortSQL;
        std::string _propertyPath;
        std::vector<std::string> _ftsProperties;
    };

}
