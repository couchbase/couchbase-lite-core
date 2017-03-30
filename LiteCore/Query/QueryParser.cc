//
//  QueryParser.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

// https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema

#include "QueryParser.hh"
#include "QueryParserTables.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Path.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <utility>
#include <algorithm>
#ifdef _MSC_VER
#include "asprintf.h"
#endif

using namespace std;
using namespace fleece;

namespace litecore {


#pragma mark - UTILITY FUNCTIONS:


    [[noreturn]] __printflike(1, 2)
    static void fail(const char *format, ...) {
        va_list args;
        va_start(args, format);
        string message = vformat(format, args);
        va_end(args);

        Warn("Invalid LiteCore query: %s", message.c_str());
        throw error(error::LiteCore, error::InvalidQuery, message);
    }

    #define require(TEST, FORMAT, ...)  if (TEST) ; else fail(FORMAT, ##__VA_ARGS__)


    template <class T>
    static T required(T val, const char *name, const char *message = "is missing") {
        require(val, "%s %s", name, message);
        return val;
    }


    static const Array* requiredArray(const Value *v, const char *what) {
        return required(required(v, what)->asArray(), what, "must be an array");
    }

    static const Dict* requiredDict(const Value *v, const char *what) {
        return required(required(v, what)->asDict(), what, "must be a dictionary");
    }

    static slice requiredString(const Value *v, const char *what) {
        return required(required(v, what)->asString(), what, "must be a string");
    }

    
    static bool isAlphanumericOrUnderscore(slice str) {
        if (str.size == 0)
            return false;
        for (size_t i = 0; i < str.size; i++)
            if (!isalnum(str[i]) && str[i] != '_')
                return false;
        return true;
    }

    static bool isValidIdentifier(slice str) {
        return isAlphanumericOrUnderscore(str) && !isdigit(str[0]);
    }

    static const Value* getCaseInsensitive(const Dict *dict, slice key) {
        for (Dict::iterator i(dict); i; ++i)
            if (i.key()->asString().caseEquivalent(key))
                return i.value();
        return nullptr;
    }


    static inline std::ostream& operator<< (std::ostream& o, slice s) {
        o.write((const char*)s.buf, s.size);
        return o;
    }


    // Writes a string with SQL quoting (inside apostrophes, doubling contained apostrophes.)
    /*static*/ void QueryParser::writeSQLString(std::ostream &out, slice str) {
        out << "'";
        bool simple = true;
        for (unsigned i = 0; i < str.size; i++) {
            if (str[i] == '\'') {
                simple = false;
                break;
            }
        }
        if (simple) {
            out.write((const char*)str.buf, str.size);
        } else {
            for (unsigned i = 0; i < str.size; i++) {
                if (str[i] == '\'')
                    out.write("''", 2);
                else
                    out.write((const char*)&str[i], 1);
            }
        }
        out << "'";
    }

    
    static string propertyFromOperands(Array::iterator &operands);
    static string propertyFromNode(const Value *node);


#pragma mark - QUERY PARSER TOP LEVEL:


    void QueryParser::reset() {
        _context.clear();
        _context.push_back(&kOuterOperation);
        _parameters.clear();
        _variables.clear();
        _ftsTables.clear();
        _1stCustomResultCol = 0;
        _isAggregateQuery = _aggregatesOK = false;
    }


    void QueryParser::parseJSON(slice expressionJSON) {
        alloc_slice expressionFleece = JSONConverter::convertJSON(expressionJSON);
        return parse(Value::fromTrustedData(expressionFleece));
    }
    
    
    void QueryParser::parse(const Value *expression) {
        reset();
        if (expression->asDict()) {
            // Given a dict; assume it's the operands of a SELECT:
            writeSelect(expression->asDict());
        } else {
            const Array *a = expression->asArray();
            if (a && a->count() > 0 && a->get(0)->asString() == "SELECT"_sl) {
                // Given an entire SELECT statement:
                parseNode(expression);
            } else {
                // Given some other expression; treat it as a WHERE clause of an implicit SELECT:
                writeSelect(expression, Dict::kEmpty);
            }
        }
    }


    void QueryParser::parseJustExpression(const Value *expression) {
        reset();
        parseNode(expression);
    }


#pragma mark - SELECT STATEMENT

    
    void QueryParser::writeSelect(const Dict *operands) {
        writeSelect(getCaseInsensitive(operands, "WHERE"_sl), operands);
    }


    void QueryParser::writeSelect(const Value *where, const Dict *operands) {
        // Have to find all properties involved in MATCH before emitting the FROM clause:
        if (where)
            findFTSProperties(where);

        // Find all the joins in the FROM clause first, to populate _aliases. This has to be done
        // before writing the WHAT clause, because that will depend on _aliases.
        auto from = getCaseInsensitive(operands, "FROM"_sl);
        if (from)
            parseFromClause(from);

        _sql << "SELECT ";

        // DISTINCT:
        auto distinct = getCaseInsensitive(operands, "DISTINCT"_sl);
        if (distinct && distinct->asBool())
            _sql << "DISTINCT ";

        // WHAT clause:
        // Default result columns:
        string defaultTablePrefix;
        if (!_aliases.empty())
            defaultTablePrefix = _aliases[0] + ".";
        int nCol = 0;
        for (auto &col : _baseResultColumns)
            _sql << (nCol++ ? ", " : "") << defaultTablePrefix << col;
        for (auto ftsTable : _ftsTables) {
            _sql << (nCol++ ? ", " : "") << "offsets(\"" << ftsTable << "\")";
        }
        _1stCustomResultCol = nCol;

        nCol += writeSelectListClause(operands, "WHAT"_sl, (nCol ? ", " : ""), true);
        require(nCol > 0, "No result columns");

        // FROM clause:
        writeFromClause(from);

        // WHERE clause:
        if (where) {
            _sql << " WHERE ";
            parseNode(where);
        }

        // GROUP_BY clause:
        bool grouped = (writeSelectListClause(operands, "GROUP_BY"_sl, " GROUP BY ") > 0);
        if (grouped)
            _isAggregateQuery = true;

        // HAVING clause:
        auto having = getCaseInsensitive(operands, "HAVING"_sl);
        if (having) {
            require(grouped, "HAVING requires GROUP_BY");
            _sql << " HAVING ";
            _aggregatesOK = true;
            parseNode(having);
            _aggregatesOK = false;
        }

        // ORDER_BY clause:
        writeSelectListClause(operands, "ORDER_BY"_sl, " ORDER BY ", true);

        // LIMIT, OFFSET clauses:
        // TODO: Use the ones from operands
        if (!_defaultLimit.empty())
            _sql << " LIMIT " << _defaultLimit;
        if (!_defaultOffset.empty())
            _sql << " OFFSET " << _defaultOffset;
    }


    // Writes a SELECT statement's 'WHAT', 'GROUP BY' or 'ORDER BY' clause:
    unsigned QueryParser::writeSelectListClause(const Dict *operands,
                                                slice key,
                                                const char *sql,
                                                bool aggregatesOK)
    {
        auto param = getCaseInsensitive(operands, key);
        if (!param) return 0;
        auto list = requiredArray(param, "WHAT / GROUP BY / ORDER BY parameter");
        int count = list->count();
        if (count == 0) return 0;

        _sql << sql;
        _context.push_back(&kExpressionListOperation); // suppresses parens around arg list
        Array::iterator items(list);
        _aggregatesOK = aggregatesOK;
        writeColumnList(items);
        _aggregatesOK = false;
        _context.pop_back();
        return count;
    }


    void QueryParser::writeCreateIndex(const Array *expressions) {
        reset();
        _sql << "CREATE INDEX IF NOT EXISTS \"" << indexName(expressions) << "\" ON " << _tableName << " ";
        Array::iterator iter(expressions);
        writeColumnList(iter);
        // TODO: Add 'WHERE' clause for use with SQLite 3.15+
    }


    void QueryParser::writeResultColumn(const Value *val) {
        switch (val->type()) {
            case kArray:
                parseNode(val);
                return;
            case kString: {
                slice str = val->asString();
                if (str == "*"_sl) {
                    fail("'*' result column isn't supported");
                    return;
                } else {
                    // "."-prefixed string becomes a property
                    writeStringLiteralAsProperty(str);
                    return;
                }
                break;
            }
            default:
                break;
        }
        fail("Invalid item type in WHAT clause; must be array or '*' or '.property'");
    }


    void QueryParser::writeStringLiteralAsProperty(slice str) {
        require(str.size > 0 && str[0] == '.',
                "Invalid property name '%.*s'; must start with '.'", SPLAT(str));
        str.moveStart(1);
        writePropertyGetter("fl_value", str.asString());
    }


#pragma mark - "FROM" / "JOIN" clauses:


    void QueryParser::parseFromClause(const Value *from) {
        for (Array::iterator i(requiredArray(from, "FROM value")); i; ++i) {
            auto entry = requiredDict(i.value(), "FROM item");
            string alias = requiredString(getCaseInsensitive(entry, "AS"_sl),
                                          "AS in FROM item").asString();
            require(isAlphanumericOrUnderscore(alias), "AS value");
            _aliases.push_back(alias);
        }
    }


    void QueryParser::writeFromClause(const Value *from) {
        _sql << " FROM " << _tableName;
        if (from) {
            for (unsigned i = 0; i < _aliases.size(); ++i) {
                auto entry = from->asArray()->get(i)->asDict();
                auto on = getCaseInsensitive(entry, "ON"_sl);
                if (i == 0) {
                    require(!on, "first FROM item cannot have an ON clause");
                    _sql << " AS \"" << _aliases[i] << "\"";
                } else {
                    require(on, "FROM item needs an ON clause to be a join");
                    if (i > 1)
                        _sql << ",";
                    auto joinType = getCaseInsensitive(entry, "JOIN"_sl);
                    if (joinType) {
                        auto typeStr = requiredString(joinType, "JOIN value").asString();
                        require(isValidJoinType(typeStr), "Unknown JOIN type '%s'", typeStr.c_str());
                        _sql << " " << typeStr;
                    }
                    _sql << " JOIN " << _tableName << " AS \"" << _aliases[i] << "\" ON ";
                    parseNode(on);
                }
            }
        }
        unsigned ftsTableNo = 0;
        for (auto ftsTable : _ftsTables) {
            _sql << ", \"" << ftsTable << "\" AS FTS" << ++ftsTableNo;
        }
    }


    bool QueryParser::isValidJoinType(const string &str) {
        for (int i = 0; i < sizeof(kJoinTypes) / sizeof(kJoinTypes[0]); ++i) {
            if (strcasecmp(kJoinTypes[i], str.c_str()) == 0)
                return true;
        }
        return false;
    }


#pragma mark - PARSING THE "WHERE" CLAUSE:
    
    
    void QueryParser::parseNode(const Value *node) {
        switch (node->type()) {
            case kNull:
                _sql << "x''";        // Represent a Fleece/JSON/N1QL null as an empty blob (?)
                break;
            case kNumber:
                _sql << node->toString();
                break;
            case kBoolean:
                _sql << (node->asBool() ? '1' : '0');    // SQL doesn't have true/false
                break;
            case kString: {
                slice str = node->asString();
                if (_context.back() == &kColumnListOperation)
                    writeStringLiteralAsProperty(str);
                else
                    writeSQLString(str);
                break;
            }
            case kData:
                fail("Binary data not supported in query");
            case kArray:
                parseOpNode((const Array*)node);
                break;
            case kDict:
                fail("Dictionaries not supported in query");
                break;
        }
    }


    void QueryParser::parseOpNode(const Array *node) {
        Array::iterator array(node);
        require(array.count() > 0, "Empty JSON array");
        slice op = requiredString(array[0], "operation");
        ++array;

        // Look up the handler:
        int nargs = min(array.count(), 9u);
        bool nameMatched = false;
        const Operation *def;
        for (def = kOperationList; def->op; ++def) {
            if (op.caseEquivalent(def->op)) {
                nameMatched = true;
                if (nargs >= def->minArgs && nargs <= def->maxArgs)
                    break;
            }
        }
        if (nameMatched && !def->op)
            fail("Wrong number of arguments to %.*s", SPLAT(op));
        handleOperation(def, op, array);
    }


    // Invokes an Operation's handler. Pushes Operation on the stack and writes parens if needed
    void QueryParser::handleOperation(const Operation* op,
                                      slice actualOperator,
                                      Array::iterator& operands)
    {
        bool parenthesize = (op->precedence <= _context.back()->precedence);
        _context.push_back(op);
        if (parenthesize)
            _sql << '(';

        auto handler = op->handler;
        (this->*handler)(actualOperator, operands);

        if (parenthesize)
            _sql << ')';
        _context.pop_back();
    }


#pragma mark - OPERATION HANDLERS:


    // Handles prefix (unary) operators
    void QueryParser::prefixOp(slice op, Array::iterator& operands) {
        _sql << op;
        if (isalpha(op[op.size-1]))
            _sql << ' ';
        parseNode(operands[0]);
    }


    // Handles postfix operators
    void QueryParser::postfixOp(slice op, Array::iterator& operands) {
        parseNode(operands[0]);
        _sql << " " << op;
    }

    
    // Handles infix operators
    void QueryParser::infixOp(slice op, Array::iterator& operands) {
        int n = 0;
        for (auto &i = operands; i; ++i) {
            if (n++ > 0) {
                if (op != ","_sl)           // special case for argument lists
                    _sql << ' ';
                _sql << op << ' ';
            }
            parseNode(i.value());
        }
    }


    // Handles EXISTS
    void QueryParser::existsOp(slice op, Array::iterator& operands) {
        // "EXISTS propertyname" turns into a call to fl_exists()
        if (writeNestedPropertyOpIfAny("fl_exists", operands))
            return;

        _sql << "EXISTS";
        if (isalpha(op[op.size-1]))
            _sql << ' ';
        parseNode(operands[0]);
    }

    
    // Handles "x BETWEEN y AND z" expressions
    void QueryParser::betweenOp(slice op, Array::iterator& operands) {
        parseNode(operands[0]);
        _sql << ' ' << op << ' ';
        parseNode(operands[1]);
        _sql << " AND ";
        parseNode(operands[2]);
    }


    // Handles "x IN y" and "x NOT IN y" expressions
    void QueryParser::inOp(slice op, Array::iterator& operands) {
        parseNode(operands.value());
        _sql << ' ' << op << ' ';
        writeArgList(++operands);
    }


    // Handles "property MATCH pattern" expressions (FTS)
    void QueryParser::matchOp(slice op, Array::iterator& operands) {
        // Write the match expression (using an implicit join):
        auto ftsTableNo = FTSPropertyIndex(operands[0]);
        Assert(ftsTableNo > 0);
        _sql << "(FTS" << ftsTableNo << ".text MATCH ";
        parseNode(operands[1]);
        _sql << " AND FTS" << ftsTableNo << ".rowid = " << _tableName << ".sequence)";
    }


    // Handles "ANY var IN array SATISFIES expr" (and EVERY, and ANY AND EVERY)
    void QueryParser::anyEveryOp(slice op, Array::iterator& operands) {
        auto var = (string)requiredString(operands[0], "ANY/EVERY first parameter");
        require(isValidIdentifier(var),
                "ANY/EVERY first parameter must be an identifier; '%s' is not", var.c_str());
        require(_variables.count(var) == 0, "Variable '%s' is already in use", var.c_str());
        _variables.insert(var);

        string property = propertyFromNode(operands[1]);
        require(!property.empty(), "ANY/EVERY only supports a property as its source");

        bool every = !op.caseEquivalent("ANY"_sl);
        bool anyAndEvery = op.caseEquivalent("ANY AND EVERY"_sl);

        //OPT: If expr is `var = value`, can generate `fl_contains(array, value)` instead 

        if (anyAndEvery) {
            _sql << '(';
            writePropertyGetter("fl_count", property);
            _sql << " > 0 AND ";
        }

        if (every)
            _sql << "NOT ";
        _sql << "EXISTS (SELECT 1 FROM ";
        writePropertyGetter("fl_each", property);
        _sql << " AS _" << var << " WHERE ";
        if (every)
            _sql << "NOT (";
        parseNode(operands[2]);
        if (every)
            _sql << ')';
        _sql << ')';
        if (anyAndEvery)
            _sql << ')';

        _variables.erase(var);
    }


    // Handles doc property accessors, e.g. [".", "prop"] or [".prop"] --> fl_value(body, "prop")
    void QueryParser::propertyOp(slice op, Array::iterator& operands) {
        writePropertyGetter("fl_value", propertyFromOperands(operands));
    }


    // Handles substituted query parameters, e.g. ["$", "x"] or ["$x"] --> $_x
    void QueryParser::parameterOp(slice op, Array::iterator& operands) {
        alloc_slice parameter;
        if (op.size == 1) {
            parameter = operands[0]->toString();
        } else {
            op.moveStart(1);
            parameter = op;
            require(operands.count() == 0, "extra operands to '%.*s'", SPLAT(parameter));
        }
        auto paramStr = (string)parameter;
        require(isAlphanumericOrUnderscore(parameter),
                "Invalid query parameter name '%.*s'", SPLAT(parameter));
        _parameters.insert(paramStr);
        _sql << "$_" << paramStr;
    }


    // Handles variables used in ANY/EVERY predicates
    void QueryParser::variableOp(slice op, Array::iterator& operands) {
        string var;
        if (op.size == 1) {
            var = (string)operands[0]->asString();
            ++operands;
        } else {
            op.moveStart(1);
            var = op.asString();
        }
        require(isValidIdentifier(var), "Invalid variable name '%.*s'", SPLAT(op));
        require(_variables.count(var) > 0, "No such variable '%.*s'", SPLAT(op));

        if (operands.count() == 0) {
            _sql << '_' << var << ".value";
        } else {
            auto property = propertyFromOperands(operands);
            _sql << "fl_value(_" << var << ".pointer, ";
            writeSQLString(_sql, slice(property));
            _sql << ")";
        }
    }


    // Handles MISSING, which is the N1QL equivalent of NULL
    void QueryParser::missingOp(slice op, Array::iterator& operands) {
        _sql << "NULL";
    }


    // Handles CASE
    void QueryParser::caseOp(fleece::slice op, Array::iterator &operands) {
        // First operand is either the expression being tested, or null if there isn't one.
        // After that, operands come in pairs of 'when', 'then'.
        // If there's one remaining, it's the 'else'.
        _sql << "CASE";
        if (operands[0]->type() != kNull) {
            _sql << ' ';
            parseNode(operands[0]);
        }
        ++operands;
        while(operands) {
            auto test = operands.value();
            ++operands;
            if (operands) {
                _sql << " WHEN ";
                parseNode(test);
                _sql << " THEN ";
                parseNode(operands.value());
                ++operands;
            } else {
                _sql << " ELSE ";
                parseNode(test);
            }
        }
        _sql << " END";
    }
    
    
    // Handles SELECT
    void QueryParser::selectOp(fleece::slice op, Array::iterator &operands) {
        // SELECT is unusual in that its operands are encoded as an object
        auto dict = requiredDict(operands[0], "Argument to SELECT");
        if (_context.size() <= 2) {
            // Outer SELECT
            writeSelect(dict);
        } else {
            // Nested SELECT; use a fresh parser
            QueryParser nested(_tableName, _bodyColumnName);
            nested.parse(dict);
            _sql << nested.SQL();
        }
    }


    // Handles unrecognized operators, based on prefix ('.', '$', '?') or suffix ('()').
    void QueryParser::fallbackOp(slice op, Array::iterator& operands) {
        // Put the actual op into the context instead of a null
        auto operation = *_context.back();
        operation.op = op;
        _context.back() = &operation;

        if (op.size > 0 && op[0] == '.') {
            op.moveStart(1);  // skip '.'
            writePropertyGetter("fl_value", string(op));
        } else if (op.size > 0 && op[0] == '$') {
            parameterOp(op, operands);
        } else if (op.size > 0 && op[0] == '?') {
            variableOp(op, operands);
        } else if (op.size > 2 && op[op.size-2] == '(' && op[op.size-1] == ')') {
            functionOp(op, operands);
        } else {
            fail("Unknown operator '%.*s'", SPLAT(op));
        }
    }


    // Handles function calls, where the op ends with "()"
    void QueryParser::functionOp(slice op, Array::iterator& operands) {
        // Look up the function name:
        op.shorten(op.size - 2);
        string fnName = op.asString();
        const FunctionSpec *spec;
        for (spec = kFunctionList; spec->name; ++spec) {
            if (op.caseEquivalent(spec->name))
                break;
        }
        require(spec->name, "Unknown function '%.*s'", SPLAT(op));
        if (spec->aggregate) {
            require(_aggregatesOK,
                    "Cannot use aggregate function %.*s() in this context", SPLAT(op));
            _isAggregateQuery = true;
        }
        auto arity = operands.count();
        require(arity >= spec->minArgs,
                "Too few arguments for function '%.*s'", SPLAT(op));
        require(arity <= spec->maxArgs || spec->maxArgs >= 9,
                "Too many arguments for function '%.*s'", SPLAT(op));

        if (spec->sqlite_name)
            op = spec->sqlite_name;
        else
            op = spec->name; // canonical case

        // Special case: "array_count(propertyname)" turns into a call to fl_count:
        if (op.caseEquivalent("array_count"_sl) && writeNestedPropertyOpIfAny("fl_count", operands))
            return;
        else if (op.caseEquivalent("rank"_sl)) {
            if (writeNestedPropertyOpIfAny("rank", operands))
                return;
            else
                fail("rank() can only be called on FTS-indexed properties");
        }

        _sql << op;
        writeArgList(operands);
    }


    // Writes operands as a comma-separated list (parenthesized depending on current precedence)
    void QueryParser::writeArgList(Array::iterator& operands) {
        handleOperation(&kArgListOperation, kArgListOperation.op, operands);
    }

    void QueryParser::writeColumnList(Array::iterator& operands) {
        handleOperation(&kColumnListOperation, kColumnListOperation.op, operands);
    }


#pragma mark - PROPERTIES:


    // Concatenates property operands to produce the property path string
    static string propertyFromOperands(Array::iterator &operands) {
        stringstream property;
        int n = 0;
        for (auto &i = operands; i; ++i,++n) {
            auto item = i.value();
            auto arr = item->asArray();
            if (arr) {
                require(n > 0, "Property path can't start with an array index");
                // TODO: Support ranges (2 numbers)
                require(arr->count() == 1, "Property array index must have exactly one item");
                require(arr->get(0)->isInteger(), "Property array index must be an integer");
                auto index = arr->get(0)->asInt();
                property << '[' << index << ']';
            } else {
                slice name = item->asString();
                require(name, "Invalid JSON value in property path");
                if (n > 0)
                    property << '.';
                property << name;
            }
        }
        return property.str();
    }


    // Returns the property represented by a node, or "" if it's not a property node
    static string propertyFromNode(const Value *node) {
        Array::iterator i(node->asArray());
        if (i.count() >= 1) {
            auto op = i[0]->asString();
            if (op && op[0] == '.') {
                if (op.size == 1) {
                    ++i;  // skip "." item
                    return propertyFromOperands(i);
                } else {
                    op.moveStart(1);
                    return (string)op;
                }
            }
        }
        return "";              // not a valid property node
    }


    // If the first operand is a property operation, writes it using the given SQL function name
    // and returns true; else returns false.
    bool QueryParser::writeNestedPropertyOpIfAny(const char *fnName, Array::iterator &operands) {
        if (operands.count() == 0 )
            return false;
        auto property = propertyFromNode(operands[0]);
        if (property.empty())
            return false;
        writePropertyGetter(fnName, property);
        return true;
    }


    // Writes a call to a Fleece SQL function, including the closing ")".
    void QueryParser::writePropertyGetter(const string &fn, string property) {
        string tableName;
        if (!_aliases.empty()) {
            // Interpret the first component of the property as a db alias:
            auto dot = property.find('.');
            auto bra = property.find('[');
            require(dot != string::npos && (bra == string::npos || dot < bra),
                    "Missing database alias name in property '%s'", property.c_str());
            string first = property.substr(0, dot);
            require(find(_aliases.begin(), _aliases.end(), first) != _aliases.end(),
                    "Unknown database alias name in property '%s'", property.c_str());
            tableName = "\"" + first + "\".";
            property = property.substr(dot+1);
        }

        if (property == "_id") {
            require(fn == "fl_value", "can't use '_id' in this context");
            _sql << tableName << "key";
        } else if (property == "_sequence") {
            require(fn == "fl_value", "can't use '_sequence' in this context");
            _sql << tableName << "sequence";
        } else if (fn == "rank") {
            // FTS rank() needs special treatment
            string fts = FTSIndexName(property);
            if (find(_ftsTables.begin(), _ftsTables.end(), fts) == _ftsTables.end())
                fail("rank() can only be called on FTS-indexed properties");
            _sql << "rank(matchinfo(\"" << fts << "\"))";
        } else {
            _sql << fn << "(" << tableName << _bodyColumnName << ", ";
            writeSQLString(_sql, slice(property));
            _sql << ")";
        }
    }


    /*static*/ std::string QueryParser::expressionSQL(const fleece::Value* expr,
                                                      const char *bodyColumnName)
    {
        QueryParser qp("XXX", bodyColumnName);
        qp.parseJustExpression(expr);
        return qp.SQL();
    }


#pragma mark - FULL-TEXT-SEARCH MATCH:


    void QueryParser::findFTSProperties(const Value *node) {
        Array::iterator i(node->asArray());
        if (i.count() == 0)
            return;
        slice op = i.value()->asString();
        ++i;
        if (op.caseEquivalent("MATCH"_sl) && i) {
            FTSPropertyIndex(i.value(), true); // add LHS
            ++i;
        }

        // Recurse into operands:
        for (; i; ++i)
            findFTSProperties(i.value());
    }


    string QueryParser::indexName(const Array *keys) const {
        string name = keys->toJSON().asString();
        for (int i = (int)name.size(); i >= 0; --i) {
            if (name[i] == '"')
                name[i] = '\'';
        }
        return _tableName + "::" + name;
    }

    
    string QueryParser::FTSIndexName(const Value *key) const {
        slice op = requiredArray(key, "left-hand side of MATCH expression")->get(0)->asString();
        require(op.size > 0, "Invalid left-hand-side of MATCH");
        if (op[0] == '.')
            return FTSIndexName(propertyFromNode(key));     // abbreviation for common case
        else
            return _tableName + "::" + indexName(key->asArray());
    }

    string QueryParser::FTSIndexName(const string &property) const {
        return _tableName + "::." + property;
    }


    size_t QueryParser::FTSPropertyIndex(const Value *matchLHS, bool canAdd) {
        string key = FTSIndexName(matchLHS);
        auto i = find(_ftsTables.begin(), _ftsTables.end(), key);
        if (i != _ftsTables.end()) {
            return i - _ftsTables.begin() + 1;
        } else if (canAdd) {
            _ftsTables.push_back(key);
            return _ftsTables.size();
        } else {
            return 0;
        }
    }

}
