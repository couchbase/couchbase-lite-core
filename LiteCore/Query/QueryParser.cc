//
// QueryParser.cc
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

// https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema

#include "QueryParser.hh"
#include "QueryParserTables.hh"
#include "Record.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Path.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "PlatformIO.hh"
#include <utility>
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore {


    // Names of the SQLite functions we register for working with Fleece data,
    // in SQLiteFleeceFunctions.cc:
    static constexpr slice kValueFnName = "fl_value"_sl;
    static constexpr slice kNestedValueFnName = "fl_nested_value"_sl;
    static constexpr slice kRootFnName  = "fl_root"_sl;
    static constexpr slice kEachFnName  = "fl_each"_sl;
    static constexpr slice kCountFnName = "fl_count"_sl;
    static constexpr slice kExistsFnName= "fl_exists"_sl;
    static constexpr slice kResultFnName= "fl_result"_sl;

    // Existing SQLite FTS rank function:
    static constexpr slice kRankFnName  = "rank"_sl;

    static constexpr slice kArrayCountFnName = "array_count"_sl;


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
            out << str;
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
        alloc_slice expressionFleece;
        try {
            expressionFleece = JSONConverter::convertJSON(expressionJSON);
        } catch (FleeceException x) {
            fail("JSON parse error: %s", x.what());
        }
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
        if (where) {
            unsigned numMatches = findFTSProperties(where);
            require(numMatches <= _ftsTables.size(),
                    "Sorry, multiple MATCHes of the same property are not allowed");
            if (numMatches > 0)
                _baseResultColumns.push_back(_tableName + ".rowid");
        }

        // Find all the joins in the FROM clause first, to populate _aliases. This has to be done
        // before writing the WHAT clause, because that will depend on _aliases.
        auto from = getCaseInsensitive(operands, "FROM"_sl);
        if (from)
            parseFromClause(from);

        _sql << "SELECT ";

        // DISTINCT:
        auto distinct = getCaseInsensitive(operands, "DISTINCT"_sl);
        auto distinctVal = distinct && distinct->asBool();
        _isAggregateQuery = _isAggregateQuery || distinctVal;
        if (distinctVal)
            _sql << "DISTINCT ";

        // WHAT clause:
        // Default result columns:
        string defaultTablePrefix;
        if (!_aliases.empty())
            defaultTablePrefix = _aliases[0] + ".";
        int nCol = 0;
        
        if(!distinctVal) {
            for (auto &col : _baseResultColumns)
                _sql << (nCol++ ? ", " : "") << defaultTablePrefix << col;
        }

        for (auto ftsTable : _ftsTables) {
            _sql << (nCol++ ? ", " : "") << "offsets(\"" << ftsTable << "\")";
        }
        _1stCustomResultCol = nCol;

        auto nCustomCol = writeSelectListClause(operands, "WHAT"_sl, (nCol ? ", " : ""), true);

        if (nCustomCol == 0) {
            // If no return columns are specified, add the docID and sequence as defaults
            if (nCol > 0)
                _sql << ", ";
            _sql << defaultTablePrefix << "key, " << defaultTablePrefix << "sequence";
        }

        // FROM clause:
        writeFromClause(from);

        // WHERE clause:
        writeWhereClause(where);

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
        writeOrderOrLimitClause(operands, "LIMIT"_sl,  "LIMIT");
        writeOrderOrLimitClause(operands, "OFFSET"_sl, "OFFSET");
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
        if (key == "WHAT"_sl)
            handleOperation(&kResultListOperation, kResultListOperation.op, items);
        else
            writeColumnList(items);
        _aggregatesOK = false;
        _context.pop_back();
        return count;
    }


    void QueryParser::writeWhereClause(const Value *where) {
        if (_includeDeleted) {
            if (where) {
                _sql << " WHERE ";
                parseNode(where);
            }
        } else {
            _sql << " WHERE ";
            if (where) {
                _sql << "(";
                parseNode(where);
                _sql << ") AND ";
            }
            writeNotDeletedTest(0);
        }
    }


    void QueryParser::writeNotDeletedTest(unsigned tableIndex) {
        _sql << '(';
        if (_aliases.empty())
            Assert(tableIndex == 0);
        else
            _sql << '"' << _aliases[tableIndex] << "\".";
        _sql << "flags & " << (unsigned)DocumentFlags::kDeleted << ") = 0";

    }


    void QueryParser::writeCreateIndex(const string &name, const Array *expressions) {
        reset();
        _sql << "CREATE INDEX \"" << name << "\" ON " << _tableName << " ";
        Array::iterator iter(expressions);
        writeColumnList(iter);
        // TODO: Add 'WHERE' clause for use with SQLite 3.15+
    }


    void QueryParser::writeOrderOrLimitClause(const Dict *operands,
                                              slice jsonKey,
                                              const char *sqlKeyword) {
        auto value = getCaseInsensitive(operands, jsonKey);
        if (value) {
            _sql << " " << sqlKeyword << " MAX(0, ";
            parseNode(value);
            _sql << ")";
        }
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
        unsigned i = 0;
        if (from) {
            for (i = 0; i < _aliases.size(); ++i) {
                auto entry = from->asArray()->get(i)->asDict();
                auto on = getCaseInsensitive(entry, "ON"_sl);
                if (i == 0) {
                    require(!on, "first FROM item cannot have an ON clause");
                    _sql << " AS \"" << _aliases[i] << "\"";
                } else {
                    JoinType joinType = kInner;
                    const Value* joinTypeVal = getCaseInsensitive(entry, "JOIN"_sl);
                    if (joinTypeVal) {
                        slice joinTypeStr = requiredString(joinTypeVal, "JOIN value");
                        joinType = JoinType(parseJoinType(joinTypeStr));
                        require(joinType != kInvalidJoin, "Unknown JOIN type '%.*s'",
                                SPLAT(joinTypeStr));
                    }

                    if (joinType == kCross) {
                        require(!on, "CROSS JOIN cannot accept an ON clause");
                    } else {
                        require(on, "FROM item needs an ON clause to be a join");
                    }

                    // Substitute CROSS for INNER join to work around SQLite loop-ordering (#379)
                    _sql << " " << kJoinTypeNames[ (joinType == kInner) ? kCross : joinType ];
                    
                    _sql << " JOIN " << _tableName << " AS \"" << _aliases[i] << "\"";

                    if (on || !_includeDeleted) {
                        _sql << " ON ";
                        if (on) {
                            if (!_includeDeleted)
                                _sql << "(";
                            parseNode(on);
                            if (!_includeDeleted) {
                                _sql << ") AND ";
                            }
                        }
                        if(!_includeDeleted) {
                            writeNotDeletedTest(i);
                        }
                    }
                }
            }
        }
        unsigned ftsTableNo = 0;
        for (auto ftsTable : _ftsTables) {
            ++ftsTableNo;
            if (i > 1)
                _sql << ",";
            _sql << " JOIN \"" << ftsTable << "\" AS FTS" << ftsTableNo
                 << " ON FTS" << ftsTableNo << ".docid = kv_default.rowid";
        }
    }


    int /*JoinType*/ QueryParser::parseJoinType(slice str) {
        for (int i = 0; kJoinTypeNames[i]; ++i) {
            if (str.caseEquivalent(slice(kJoinTypeNames[i])))
                return i;  // really returns JoinType
        }
        return kInvalidJoin;
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
            case kString:
                parseStringLiteral(node->asString());
                break;
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


    // Like parseNode(), but adds a SQL `COLLATE` operator if a collation is in effect and has not
    // yet been written into the SQL.
    void QueryParser::parseCollatableNode(const Value *node) {
        if (_collationUsed) {
            parseNode(node);
        } else {
            _collationUsed = true;
            // enforce proper parenthesization; SQL COLLATE has super high precedence
            _context.push_back(&kHighPrecedenceOperation);
            parseNode(node);
            _context.pop_back();
            writeCollation();
        }
    }


    void QueryParser::writeCollation() {
        _sql << " COLLATE " << _collation.sqliteName();
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


    // Handles a node that's a string. It's treated as a string literal, except in the context of
    // a column-list ('FROM', 'ORDER BY', creating index, etc.) where it's a property path.
    void QueryParser::parseStringLiteral(slice str) {
        if (_context.back() == &kColumnListOperation || _context.back() == &kResultListOperation) {
            require(str.size > 0 && str[0] == '.',
                    "Invalid property name '%.*s'; must start with '.'", SPLAT(str));
            str.moveStart(1);
            writePropertyGetter(kValueFnName, str.asString());
        } else {
            writeSQLString(str);
        }
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
            // Write the operation/delimiter between arguments
            if (n++ > 0) {
                if (op != ","_sl)           // special case for argument lists
                    _sql << ' ';
                _sql << op << ' ';
            }
            parseCollatableNode(i.value());
        }
    }


    // Handles the WHAT clause (list of results)
    void QueryParser::resultOp(slice op, Array::iterator& operands) {
        int n = 0;
        for (auto &i = operands; i; ++i) {
            // Write the operation/delimiter between arguments
            if (n++ > 0)
                _sql << ", ";
            _sql << kResultFnName << "(";
            parseCollatableNode(i.value());
            _sql << ")";
        }
    }


    // Handles array literals (the "[]" op)
    // But note that this op is treated specially if it's an operand of "IN" (see inOp)
    void QueryParser::arrayLiteralOp(slice op, Array::iterator& operands) {
        functionOp("array_of()"_sl, operands);
    }

    // Handles EXISTS
    void QueryParser::existsOp(slice op, Array::iterator& operands) {
        // "EXISTS propertyname" turns into a call to fl_exists()
        if (writeNestedPropertyOpIfAny(kExistsFnName, operands))
            return;

        _sql << "EXISTS";
        if (isalpha(op[op.size-1]))
            _sql << ' ';
        parseNode(operands[0]);
    }


    static void setFlagFromOption(bool &flag, const Dict *options, slice key) {
        const Value *val = getCaseInsensitive(options, key);
        if (val)
            flag = val->asBool();
    }

    
    // Handles COLLATE
    void QueryParser::collateOp(slice op, Array::iterator& operands) {
        auto outerCollation = _collation;
        auto outerCollationUsed = _collationUsed;

        // Apply the collation options, overriding the inherited ones:
        const Dict *options = requiredDict(operands[0], "COLLATE options");
        setFlagFromOption(_collation.unicodeAware,       options, "UNICODE"_sl);
        setFlagFromOption(_collation.caseSensitive,      options, "CASE"_sl);
        setFlagFromOption(_collation.diacriticSensitive, options, "DIAC"_sl);

        auto localeName = getCaseInsensitive(options, "LOCALE"_sl);
        if (localeName)
            _collation.localeName = localeName->asString();
        _collationUsed = false;

        // Remove myself from the operator stack so my precedence doesn't cause confusion:
        auto curContext = _context.back();
        _context.pop_back();

        // Parse the expression:
        parseNode(operands[1]);

        // If nothing in the expression (like a comparison operator) used the collation to generate
        // a SQL 'COLLATE', generate one now for the entire expression:
        if (!_collationUsed)
            writeCollation();

        _context.push_back(curContext);

        // Pop the collation options:
        _collation = outerCollation;
        _collationUsed = outerCollationUsed;
    }


    // Handles "x BETWEEN y AND z" expressions
    void QueryParser::betweenOp(slice op, Array::iterator& operands) {
        parseCollatableNode(operands[0]);
        _sql << ' ' << op << ' ';
        parseNode(operands[1]);
        _sql << " AND ";
        parseNode(operands[2]);
    }


    // Handles "x IN y" and "x NOT IN y" expressions
    void QueryParser::inOp(slice op, Array::iterator& operands) {
        bool notIn = (op != "IN"_sl);
        auto arrayOperand = operands[1]->asArray();
        if (arrayOperand && arrayOperand->count() > 0 && arrayOperand->get(0)->asString() == "[]"_sl) {
            // RHS is a literal array, so use SQL "IN" syntax:
            parseCollatableNode(operands[0]);
            _sql << ' ' << op << ' ';
            Array::iterator arrayOperands(arrayOperand);
            writeArgList(++arrayOperands);

        } else {
            // Otherwise generate a call to array_contains():
            _context.push_back(&kArgListOperation);     // prevents extra parens around operands

            if (notIn)
                _sql << "(NOT ";

            _sql << "array_contains(";
            parseNode(operands[1]);     // yes, operands are in reverse order
            _sql << ", ";
            parseCollatableNode(operands[0]);
            _sql << ")";

            if (notIn)
                _sql << ")";

            _context.pop_back();
        }
    }


    // Handles "fts_index MATCH pattern" expressions (FTS)
    void QueryParser::matchOp(slice op, Array::iterator& operands) {
        // Is a MATCH legal here? Look at the parent operation(s):
        auto parentCtx = _context.rbegin() + 1;
        auto parentOp = (*parentCtx)->op;
        while (parentOp == "AND"_sl)
            parentOp = (*++parentCtx)->op;
        require(parentOp == "SELECT"_sl || parentOp == nullslice,
                "MATCH can only appear at top-level, or in a top-level AND");

        // Write the expression:
        auto ftsTableNo = FTSPropertyIndex(operands[0]);
        Assert(ftsTableNo > 0);
        _sql << "FTS" << ftsTableNo << ".\"" << FTSTableName(operands[0]) << "\" MATCH ";
        parseCollatableNode(operands[1]);
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
            writePropertyGetter(kCountFnName, property);
            _sql << " > 0 AND ";
        }

        if (every)
            _sql << "NOT ";
        _sql << "EXISTS (SELECT 1 FROM ";
        writePropertyGetter(kEachFnName, property);
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
        writePropertyGetter(kValueFnName, propertyFromOperands(operands));
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
        // Concatenate the op and operands as a path:
        string var;
        if (op.size > 1) {
            op.moveStart(1);
            var = op.asString();
        }
        if (operands.count() > 0) {
            if (!var.empty())
                var += '.';
            var += propertyFromOperands(operands);
        }

        // Split the path into variable name and property:
        string property;
        auto dot = var.find('.');
        if (dot != string::npos) {
            property = var.substr(dot + 1);
            var = var.substr(0, dot);
        }

        require(isValidIdentifier(var), "Invalid variable name '%.*s'", SPLAT(op));
        require(_variables.count(var) > 0, "No such variable '%.*s'", SPLAT(op));

        // Now generate the function call:
        if (property.empty()) {
            _sql << '_' << var << ".value";
        } else {
            _sql << kNestedValueFnName << "(_" << var << ".pointer, ";
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
            writePropertyGetter(kValueFnName, string(op));
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
        if (op.caseEquivalent(kArrayCountFnName) && writeNestedPropertyOpIfAny(kCountFnName, operands))
            return;

        // Special case: in "rank(ftsName)" the param has to be a matchinfo() call:
        if (op.caseEquivalent(kRankFnName)) {
            string fts = FTSTableName(operands[0]);
            if (find(_ftsTables.begin(), _ftsTables.end(), fts) == _ftsTables.end())
                fail("rank() can only be called on FTS indexes");
            _sql << "rank(matchinfo(\"" << fts << "\"))";
            return;
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
    bool QueryParser::writeNestedPropertyOpIfAny(slice fnName, Array::iterator &operands) {
        if (operands.count() == 0 )
            return false;
        auto property = propertyFromNode(operands[0]);
        if (property.empty())
            return false;
        writePropertyGetter(fnName, property);
        return true;
    }


    // Writes a call to a Fleece SQL function, including the closing ")".
    void QueryParser::writePropertyGetter(slice fn, string property) {
        string tableName;
        if (!_aliases.empty()) {
            // Interpret the first component of the property as a db alias:
            auto dot = property.find('.');
            string rest;
            if (dot == string::npos) {
                dot = property.size();
            } else {
                rest = property.substr(dot+1);
            }
            string first = property.substr(0, dot);

            // Make sure there isn't a bracket (array index) before the dot:
            auto bra = property.find('[');
            require(bra == string::npos || dot < bra,
                    "Missing database alias name in property '%s'", property.c_str());

            require(find(_aliases.begin(), _aliases.end(), first) != _aliases.end(),
                    "Unknown database alias name in property '%s'", property.c_str());
            tableName = "\"" + first + "\".";
            property = rest;
        }

        if (property == "_id") {
            require(fn == kValueFnName, "can't use '_id' in this context");
            _sql << tableName << "key";
        } else if (property == "_sequence") {
            require(fn == kValueFnName, "can't use '_sequence' in this context");
            _sql << tableName << "sequence";
        } else {
            // It's more efficent to get the doc root with fl_root than with fl_value:
            if (property == "" && fn == kValueFnName)
                fn = kRootFnName;

            // Write the function call:
            _sql << fn << "(" << tableName << _bodyColumnName;
            if(!property.empty()) {
                _sql << ", ";
                writeSQLString(_sql, slice(property));
            }
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


    // Recursively looks for MATCH expressions and adds the properties being matched to _ftsTables.
    // Returns the number of expressions found.
    unsigned QueryParser::findFTSProperties(const Value *node) {
        unsigned found = 0;
        Array::iterator i(node->asArray());
        if (i.count() == 0)
            return 0;
        slice op = i.value()->asString();
        ++i;
        if (op.caseEquivalent("MATCH"_sl) && i) {
            found = 1;
            FTSPropertyIndex(i.value(), true); // add LHS
            ++i;
        }

        // Recurse into operands:
        for (; i; ++i)
            found += findFTSProperties(i.value());
        return found;
    }


    string QueryParser::FTSTableName(const Value *key) const {
        slice ftsName = requiredString(key, "left-hand side of MATCH expression");
        return FTSTableName(string(ftsName));
    }

    string QueryParser::FTSTableName(const string &indexName) const {
        require(!indexName.empty() && indexName.find('"') == string::npos,
                "FTS index name may not contain double-quotes nor be empty");
        return _tableName + "::" + indexName;
    }

    size_t QueryParser::FTSPropertyIndex(const Value *matchLHS, bool canAdd) {
        string key = FTSTableName(matchLHS);
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


    string QueryParser::FTSColumnName(const Value *expression) {
        slice op = requiredArray(expression, "FTS index expression")->get(0)->asString();
        require(op.size > 0, "invalid FTS index expression");
        require(op[0] == '.', "FTS index expression must be a property");
        string property = propertyFromNode(expression);
        require(!property.empty(), "invalid property expression");
        return property;
    }

}
