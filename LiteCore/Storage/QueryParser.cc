//
//  QueryParser.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "QueryParser.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Path.hh"
#include <utility>

using namespace std;
using namespace fleece;

namespace litecore {


    #define TABLECOUNT(TABLE)   (sizeof(TABLE) / sizeof((TABLE)[0]))

    // Relational operators, appearing as dictionary keys, e.g. {"$eq": 42}
    struct relationalEntry {const char *op; const char *sqlOp; int type;};
    static const relationalEntry kRelationals[] = {
        {"$eq",     " = "},
        {"$ne",     " <> "},
        {"$lt",     " < "},
        {"$lte",    " <= "},
        {"$le",     " <= "},
        {"$gt",     " > "},
        {"$gte",    " >= "},
        {"$ge",     " >= "},
        {"$like",   " LIKE "},
        {"$type",   nullptr,    1},
        {"$exists", nullptr,    2},
        {"$in",     " IN ",     3},
        {"$nin",    " NOT IN ", 3},
        {"$size",   nullptr,    4},
        {"$all",    nullptr,    5},
        {"$any",    nullptr,    6},
    };

    // Names of Fleece types, indexed by fleece::valueType. Used with "$type" operator.
    static const char* const kTypeNames[] = {
        "null", "boolean", "number", "string", "blob", "array", "object"
    };
    
    
    static void fail() {
        error::_throw(error::InvalidQuery);
    }


    static const Value* mustExist(const Value *v) {
        if (!v)
            fail();
        return v;
    }

    static const Array* mustBeArray(const Value *v) {
        if (!v)
            return nullptr;
        const Array *a = v->asArray();
        if (!a)
            fail();
        return a;
    }

    static const Dict* mustBeDict(const Value *v) {
        if (!v)
            return nullptr;
        const Dict *d = v->asDict();
        if (!d)
            fail();
        return d;
    }

    // If the dict has a key starting with '$', returns it and its value; else nulls.
    static pair<slice,const Value*> getSpecialKey(const Value *val) {
        for (Dict::iterator i(val->asDict()); i; ++i) {
            slice key = i.key()->asString();
            if (key.size > 0 && key[0] == '$') {
                return {key, i.value()};
            }
        }
        return {};
    }


    class Delimiter {
    public:
        Delimiter(ostream &out, const string &word)
        :_out(out), _word(word)
        {
            //_out << "(";
        }

        void next() {
            if (_first)
                _first = false;
            else
                _out << _word;
        }

        ~Delimiter() {
            //_out << ")";
        }
    private:
        ostream &_out;
        string _word;
        bool _first {true};
    };


    static string appendPaths(const string &parent, string child) {
        if (child[0] == '$') {
            if (child[1] == '.')
                child = child.substr(2);
            else
                child = child.substr(1);
        }
        if (parent.empty())
            return child;
        else if (child[0] == '[')
            return parent + child;
        else
            return parent + "." + child;

    }


#pragma mark - QUERY PARSER:


    void QueryParser::parse(const Value* v) {
        parsePredicate(v);
    }


    /*static*/ void QueryParser::parse(const Value* v, ostream &out) {
        QueryParser qp(out);
        qp.parse(v);
    }

    /*static*/ void QueryParser::parseJSON(slice json, std::ostream &out) {
        auto fleeceData = JSONConverter::convertJSON(json);
        auto root = Value::fromTrustedData(fleeceData);
        parse(root, out);
    }


    void QueryParser::parsePredicate(const Value *q) {
        const Dict *query = mustBeDict(mustExist(q));
        auto special = getSpecialKey(query);
        if (!special.first.buf) {
            // No special operator; interpret each key as a property path with implicit 'and':
            Delimiter d(_sql, " AND ");
            for (Dict::iterator i(query); i; ++i) {
                d.next();
                parseTerm(i.key()->asString(), i.value());
            }
        } else if (special.first == slice("$and")) {
            parseBooleanExpr(special.second, " AND ");
        } else if (special.first == slice("$or")) {
            parseBooleanExpr(special.second, " OR ");
        } else if (special.first == slice("$nor")) {
            _sql << "NOT (";
            parseBooleanExpr(special.second, " OR ");
            _sql << ")";
        } else if (special.first == slice("$not")) {
            auto terms = mustBeArray(special.second);
            if (terms->count() != 1)
                fail();
            _sql << "NOT (";
            parsePredicate(terms->get(0));
            _sql << ")";
        }
    }


    void QueryParser::parseBooleanExpr(const Value *terms, const char *op) {
        Delimiter d(_sql, op);
        for (Array::iterator i(mustBeArray(mustExist(terms))); i; ++i) {
            d.next();
            parsePredicate(i.value());
        }
    }


    void QueryParser::parseTerm(slice key, const Value *value) {
        slice op;
        auto special = getSpecialKey(value);
        if (special.second) {
            op = special.first;
            value = special.second;
        } else {
            if (value->type() == kDict) {
                parseSubPropertyTerm(key, value->asDict());
                return;
            }
            op = slice("$eq");
        }

        const relationalEntry *rel = nullptr;
        for (unsigned i = 0; i < TABLECOUNT(kRelationals); ++i) {
            if (op == slice(kRelationals[i].op)) {
                rel = &kRelationals[i];
                break;
            }
        }
        if (!rel)
            fail();

        switch (rel->type) {
            case 0:     // normal binary op
                writePropertyGetter("fl_value", key);
                _sql << rel->sqlOp;
                writeLiteral(value);
                break;
            case 1: {   // $type
                writePropertyGetter("fl_type", key);
                _sql << "=";
                int typeCode = -1;
                slice typeName = value->asString();
                for (int i = 0; i < TABLECOUNT(kTypeNames); ++i) {
                    if (typeName == slice(kTypeNames[i])) {
                        typeCode = i;
                        break;
                    }
                }
                if (typeCode < 0)
                    fail();
                _sql << typeCode;
                break;
            }
            case 2:     // $exists
                if (!value->asBool())
                    _sql << "NOT ";
                writePropertyGetter("fl_exists", key);
                break;
            case 3: {   // $in, $nin
                writePropertyGetter("fl_value", key);
                _sql << rel->sqlOp << "(";
                Delimiter d(_sql, ", ");
                for (Array::iterator i(mustBeArray(value)); i; ++i) {
                    d.next();
                    writeLiteral(i.value());
                }
                _sql << ")";
                break;
            }
            case 4:     // $size
                writePropertyGetter("fl_count", key);
                _sql << "=";
                writeLiteral(value);
                break;
            case 5:     // $all
            case 6: {   // $any
                writePropertyGetterLeftOpen("fl_contains", key);
                _sql << ((rel->type == 5) ? ", 1" : ", 0");
                for (Array::iterator i(mustBeArray(value)); i; ++i) {
                    _sql << ", ";
                    writeLiteral(i.value());
                }
                _sql << ")";
                break;
            }
            default:
                CBFAssert("invalid type in kRelationals" == nullptr);
        }
    }


    void QueryParser::parseSubPropertyTerm(slice property, const Dict *value) {
        // Match the property value in the context of this property:
        auto savedPropertyPath = _propertyPath;
        _propertyPath = appendPaths(_propertyPath, (string)property);
        _sql << "(";
        parsePredicate(value);
        _sql << ")";
        _propertyPath = savedPropertyPath;
    }


    void QueryParser::writePropertyGetterLeftOpen(const char *fn, slice property) {
        _sql << fn;
        _sql << "(body, ";
        auto path = appendPaths(_propertyPath, (string)property);
        writeSQLString(slice(path));
    }


    void QueryParser::writePropertyGetter(const char *fn, slice property) {
        writePropertyGetterLeftOpen(fn, property);
        _sql << ")";
    }

    
    void QueryParser::writeLiteral(const Value *literal) {
        switch (literal->type()) {
            case kNumber: {
                alloc_slice str = literal->toString();
                _sql << string((const char*)str.buf, str.size);
                break;
            }
            case kBoolean:
                _sql << (literal->asBool() ? "1" : "0");    // SQL doesn't have true/false
                break;
            case kString:
                writeSQLString(literal->asString());
                break;
            default:
                fail();
        }
    }


    void QueryParser::writeSQLString(slice str) {
        _sql << "'";
        bool simple = true;
        for (unsigned i = 0; i < str.size; i++) {
            if (str[i] == '\'') {
                simple = false;
                break;
            }
        }
        if (simple) {
            _sql.write((const char*)str.buf, str.size);
        } else {
            for (unsigned i = 0; i < str.size; i++) {
                if (str[i] == '\'')
                    _sql.write("''", 2);
                else
                    _sql.write((const char*)&str[i], 1);
            }
        }
        _sql << "'";
   }

}
