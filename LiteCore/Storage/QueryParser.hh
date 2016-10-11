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
        void parse(const fleece::Value*);
        void parseJSON(slice json);

        std::string whereClause()               {return _sql.str();}

        static std::string propertyGetter(slice property);
        static void writeSQLString(std::ostream &out, slice str);

    private:
        void writePropertyGetter(const char *fn, slice property);
        void writeSQLString(slice str)      {writeSQLString(_sql, str);}

        void parsePredicate(const fleece::Value*);
        void parseTerm(slice key, const fleece::Value*);
        void parseSubPropertyTerm(slice key, const fleece::Dict*);
        void parseElemMatch(slice property, const fleece::Value*);
        void parseElemMatchTerm(std::string key, const fleece::Value*);
        void writeBooleanExpr(const fleece::Value *terms, const char *op);
        void writeLiteral(const fleece::Value *literal);
        void writePropertyGetterLeftOpen(const char *fn, slice property);

        std::stringstream _sql;
        std::string _propertyPath;
    };

}
