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

namespace fleece {
    class Value;
    class Array;
    class Dict;
    class Path;
}

namespace litecore {


    class QueryParser {
    public:

        QueryParser(std::ostream &out)      :_sql(out) { }

        void parse(const fleece::Value*);

        static void parse(const fleece::Value*, std::ostream &out);
        static void parseJSON(slice json, std::ostream &out);

        void writePropertyGetter(const char *fn, slice property);
        void writePropertyGetter(slice property) {writePropertyGetter("fl_value", property);}
        void writeSQLString(slice str);

    private:
        void parsePredicate(const fleece::Value*);
        void parseTerm(slice key, const fleece::Value*);
        void parseSubPropertyTerm(slice key, const fleece::Dict*);
        void parseBooleanExpr(const fleece::Value *terms, const char *op);
        void writeLiteral(const fleece::Value *literal);
        void writePropertyGetterLeftOpen(const char *fn, slice property);

        std::ostream &_sql;
        std::string _propertyPath;
    };

}
