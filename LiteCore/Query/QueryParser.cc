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
#include "QueryParser+Private.hh"
#include "QueryParserTables.hh"
#include "Record.hh"
#include "Error.hh"
#include "FleeceImpl.hh"
#include "DeepIterator.hh"
#include "Path.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "PlatformIO.hh"
#include "SecureDigest.hh"
#include <utility>
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace fleece::impl;
using namespace litecore::qp;

namespace litecore {

#pragma mark - UTILITY FUNCTIONS:

    namespace qp {
        void fail(const char *format, ...) {
            va_list args;
            va_start(args, format);
            string message = vformat(format, args);
            va_end(args);

            Warn("Invalid LiteCore query: %s", message.c_str());
            throw error(error::LiteCore, error::InvalidQuery, message);
        }


        const Array* requiredArray(const Value *v, const char *what) {
            return required(required(v, what)->asArray(), what, "must be an array");
        }

        const Dict* requiredDict(const Value *v, const char *what) {
            return required(required(v, what)->asDict(), what, "must be a dictionary");
        }

        slice requiredString(const Value *v, const char *what) {
            return required(required(v, what)->asString(), what, "must be a string");
        }

        unsigned findNodes(const Value *root, slice op, unsigned argCount,
                           function_ref<void(const Array*)> callback)
        {
            unsigned n = 0;
            for (DeepIterator di(root); di; ++di) {
                auto operation = di.value()->asArray();
                if (operation && operation->count() > argCount
                    && operation->get(0)->asString().caseEquivalent(op)) {
                    callback(operation);
                    ++n;
                }
            }
            return n;
        }
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
    static void writeEscapedString(std::ostream &out, slice str, char quote) {
        bool simple = true;
        for (unsigned i = 0; i < str.size; i++) {
            if (str[i] == quote) {
                simple = false;
                break;
            }
        }
        if (simple) {
            out << str;
        } else {
            for (unsigned i = 0; i < str.size; i++) {
                if (str[i] == quote)
                    out.write(&quote, 1);
                out.write((const char*)&str[i], 1);
            }
        }
    }


    // Writes a string with SQL quoting (inside apostrophes, doubling contained apostrophes.)
    /*static*/ void QueryParser::writeSQLString(std::ostream &out, slice str, char quote) {
        out << quote;
        writeEscapedString(out, str, quote);
        out << quote;
    }


    static string quoteTableName(const string &name) {
        if (name == kDefaultTableAlias)
            return name;
        else
            return string("\"") + name + "\"";
    }

    
#pragma mark - QUERY PARSER TOP LEVEL:


    void QueryParser::reset() {
        _sql.str(string());
        _context.clear();
        _context.push_back(&kOuterOperation);
        _parameters.clear();
        _variables.clear();
        _ftsTables.clear();
        _indexJoinTables.clear();
        _aliases.clear();
        _dbAlias.clear();
        _columnTitles.clear();
        _1stCustomResultCol = 0;
        _isAggregateQuery = _aggregatesOK = _propertiesUseAliases = _checkedExpiration = false;

        _aliases.insert({_dbAlias, kDBAlias});
    }


    void QueryParser::parseJSON(slice expressionJSON) {
        Retained<Doc> doc;
        try {
            doc = Doc::fromJSON(expressionJSON);
        } catch (FleeceException x) {
            fail("JSON parse error: %s", x.what());
        }
        return parse(doc->root());
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
        // Find all the joins in the FROM clause first, to populate alias info. This has to be done
        // before writing the WHAT clause, because that will depend on the aliases.
        auto from = getCaseInsensitive(operands, "FROM"_sl);
        parseFromClause(from);
        
        // Have to find all properties involved in MATCH before emitting the FROM clause:
        if (where) {
            unsigned numMatches = findFTSProperties(where);
            require(numMatches <= _ftsTables.size(),
                    "Sorry, multiple MATCHes of the same property are not allowed");
        }

        // Add the indexed prediction() calls to _indexJoinTables now
        findPredictionCalls(operands);

        _sql << "SELECT ";

        // DISTINCT:
        auto distinct = getCaseInsensitive(operands, "DISTINCT"_sl);
        auto distinctVal = distinct && distinct->asBool();
        if (distinctVal) {
            _sql << "DISTINCT ";
            _isAggregateQuery = true;
        }

        // WHAT clause:
        string defaultTablePrefix;
        if (_propertiesUseAliases)
            defaultTablePrefix = quoteTableName(_dbAlias) + ".";

        auto startPosOfWhat = _sql.tellp();
        _1stCustomResultCol = 0;

        auto nCustomCol = writeSelectListClause(operands, "WHAT"_sl, "", true);
        if (nCustomCol == 0) {
            // If no return columns are specified, add the docID and sequence as defaults
            _sql << defaultTablePrefix << "key, " << defaultTablePrefix << "sequence";
            _columnTitles.push_back(kDocIDProperty);
            _columnTitles.push_back(kSequenceProperty);
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

        // Now go back and prepend some WHAT columns needed for FTS:
        if(!_isAggregateQuery && !_ftsTables.empty()) {
            stringstream extra;
            extra << defaultTablePrefix << _dbAlias << ".rowid";

            // Write columns for the FTS match offsets (in order of appearance of the MATCH expressions)
            for (string &ftsTable : _ftsTables) {
                const string &alias = _indexJoinTables[ftsTable];
                extra << ", offsets(" << alias << ".\"" << ftsTable << "\")";
            }
            extra << ", ";
            string str = _sql.str();
            str.insert(startPosOfWhat, extra.str());
            _sql.str(str);
            _sql.seekp(0, stringstream::end);
            _1stCustomResultCol += 1 + _ftsTables.size();
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
        _checkedDeleted = false;
        _sql << " WHERE ";
        if (where) {
            _sql << "(";
            parseNode(where);
            _sql << ")";
        }
        if (!_checkedDeleted) {
            if (where)
                _sql << " AND ";
            writeDeletionTest(_dbAlias);
        }
    }


    void QueryParser::writeDeletionTest(const string &alias, bool isDeleted) {
        _sql << "(";
        if (!alias.empty())
            _sql << quoteTableName(alias) << '.';
        _sql << "flags & " << (unsigned)DocumentFlags::kDeleted
             << (isDeleted ? " != 0)" : " = 0)");
    }


    void QueryParser::writeCreateIndex(const string &name,
                                       Array::iterator &expressionsIter,
                                       bool isUnnestedTable)
    {
        reset();
        if (isUnnestedTable)
            _aliases[_dbAlias] = kUnnestTableAlias;
        _sql << "CREATE INDEX \"" << name << "\" ON " << _tableName << " ";
        if (expressionsIter.count() > 0) {
            writeColumnList(expressionsIter);
        } else {
            // No expressions; index the entire body (this is used with unnested/array tables):
            Assert(isUnnestedTable);
            _sql << '(' << kUnnestedValueFnName << "(" << _bodyColumnName << "))";
        }
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
        _aliases.clear();
        bool first = true;
        if (from) {
            for (Array::iterator i(requiredArray(from, "FROM value")); i; ++i) {
                if (first)
                    _propertiesUseAliases = true;
                auto entry = requiredDict(i.value(), "FROM item");
                string alias = requiredString(getCaseInsensitive(entry, "AS"_sl),
                                              "AS in FROM item").asString();
                require(isAlphanumericOrUnderscore(alias), "AS value");
                require(_aliases.find(alias) == _aliases.end(),
                        "duplicate AS identifier '%s'", alias.c_str());

                // Determine the alias type:
                auto unnest = getCaseInsensitive(entry, "UNNEST"_sl);
                auto on = getCaseInsensitive(entry, "ON"_sl);

                aliasType type;
                if (first) {
                    require(!on && !unnest, "first FROM item cannot have an ON or UNNEST clause");
                    type = kDBAlias;
                    _dbAlias = alias;
                } else if (!unnest) {
                    type = kJoinAlias;
                } else {
                    require (!on, "cannot use ON and UNNEST together");
                    string unnestTable = unnestedTableName(unnest);
                    if (_delegate.tableExists(unnestTable))
                        type = kUnnestTableAlias;
                    else
                        type = kUnnestVirtualTableAlias;
                }
                _aliases.insert({alias, type});
                first = false;
            }
        }
        if (first) {
            _dbAlias = kDefaultTableAlias;
            _aliases.insert({_dbAlias, kDBAlias});
        }
    }


    void QueryParser::writeFromClause(const Value *from) {
        auto fromArray = (const Array*)from;    // already type-checked by parseFromClause

        _sql << " FROM " << _tableName;

        if (fromArray && !fromArray->empty()) {
            for (Array::iterator i(fromArray); i; ++i) {
                auto entry = requiredDict(i.value(), "FROM item");
                string alias = requiredString(getCaseInsensitive(entry, "AS"_sl),
                                              "AS in FROM item").asString();
                auto on = getCaseInsensitive(entry, "ON"_sl);
                auto unnest = getCaseInsensitive(entry, "UNNEST"_sl);
                switch (_aliases[alias]) {
                    case kDBAlias:
                        // The first item is the database alias:
                        _sql << " AS \"" << alias << "\"";
                        break;
                    case kUnnestVirtualTableAlias:
                        // UNNEST: Use fl_each() to make a virtual table:
                        _sql << " JOIN ";
                        writeEachExpression(unnest);
                        _sql << " AS \"" << alias << "\"";
                        break;
                    case kUnnestTableAlias: {
                        // UNNEST: Optimize query by using the unnest table as a join source:
                        string unnestTable = unnestedTableName(unnest);
                        _sql << " JOIN \"" << unnestTable << "\" AS \"" << alias << "\""
                                " ON \"" << alias << "\".docid=\"" << _dbAlias << "\".rowid";
                        break;
                    }
                    case kJoinAlias: {
                        // A join:
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

                        _sql << " JOIN " << _tableName << " AS \"" << alias << "\"";

                        _sql << " ON ";
                        _checkedDeleted = false;
                        if (on) {
                            _sql << "(";
                            parseNode(on);
                            _sql << ")";
                        }
                        if (!_checkedDeleted) {
                            if (on)
                                _sql << " AND ";
                            writeDeletionTest(alias);
                        }
                        break;
                    }
                }
            }
        } else {
            _sql << " AS " << quoteTableName(_dbAlias);
        }

        // Add joins to index tables (FTS, predictive):
        for (auto &ftsTable : _indexJoinTables) {
            auto &table = ftsTable.first;
            auto &alias = ftsTable.second;
            _sql << " JOIN \"" << table << "\" AS " << alias
                 << " ON " << alias << ".docid = " << quoteTableName(_dbAlias) << ".rowid";
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
        _curNode = node;
        switch (node->type()) {
            case kNull:
                _sql << kNullFnName << "()";
                break;
            case kNumber:
                if(node->isInteger()) {
                    _sql << node->toString();
                } else {
                    // https://github.com/couchbase/couchbase-lite-core/issues/555
                    // Too much precision and stringifying is affected, but too little
                    // and the above issue happens so query parser needs to use a higher
                    // precision version of the floating point number
                    char buf[32];
                    sprintf(buf, "%.17g", node->asDouble());
                    _sql << buf;
                }
                
                break;
            case kBoolean:
                _sql << kBoolFnName << '(' << (int)node->asBool() << ')';
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
                writeDictLiteral((const Dict*)node);
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
        _sql << " COLLATE \"" << _collation.sqliteName() << "\"";
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
            writePropertyGetter(kValueFnName, propertyFromString(str));
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
        if (operands.count() >= 2 && operands[1]->type() == kNull) {
            // Ugly special case where SQLite's semantics for 'IS [NOT]' don't match N1QL's (#410)
            if (op.caseEquivalent("IS"_sl))
                op = "="_sl;
            else if (op.caseEquivalent("IS NOT"_sl))
                op = "!="_sl;
        }

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


    static string columnTitleFromProperty(const string &property) {
        if (property[0] == '_') {
            return property.substr(1);
        } else {
            auto dot = property.rfind('.');
            return (dot == string::npos) ? property : property.substr(dot+1);
        }
    }


    // Handles the WHAT clause (list of results)
    void QueryParser::resultOp(slice op, Array::iterator& operands) {
        int n = 0;
        unsigned anonCount = 0;
        for (auto &i = operands; i; ++i) {
            // Write the operation/delimiter between arguments
            auto result = i.value();
            string title;
            Array::iterator expr(result->asArray());
            if (expr && expr[0]->asString().caseEquivalent("AS"_sl)) {
                // Handle 'AS':
                require(expr.count() == 3, "'AS' must have two operands");
                title = string(requiredString(expr[2], "'AS' alias"));
                result = expr[1];
            }

            if (n++ > 0)
                _sql << ", ";
            _sql << kResultFnName << "(";
            parseCollatableNode(result);
            _sql << ")";

            // Come up with a column title if there is no 'AS':
            if (title.empty()) {
                if (result->type() == kString) {
                    title = columnTitleFromProperty(propertyFromString(result->asString()));
                } else if (result->type() == kArray && expr[0]->asString().hasPrefix('.')) {
                    title = columnTitleFromProperty(propertyFromNode(result));
                } else {
                    title = format("$%u", ++anonCount); // default for non-properties
                }
                if (title.empty())
                    title = "*";        // for the property ".", i.e. the entire doc
            }

            // Make the title unique:
            string uniqueTitle = title;
            unsigned dup = 2;
            while (find(_columnTitles.begin(), _columnTitles.end(), uniqueTitle) != _columnTitles.end())
                uniqueTitle = title + format(" #%u", dup++);
            _columnTitles.push_back(uniqueTitle);
        }
    }


    // Handles array literals (the "[]" op)
    // But note that this op is treated specially if it's an operand of "IN" (see inOp)
    void QueryParser::arrayLiteralOp(slice op, Array::iterator& operands) {
        functionOp(kArrayFnNameWithParens, operands);
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
        auto ftsTableAlias = FTSJoinTableAlias(operands[0]);
        Assert(!ftsTableAlias.empty());
        _sql << ftsTableAlias << ".\"" << FTSTableName(operands[0]) << "\" MATCH ";
        parseCollatableNode(operands[1]);
    }


    // Handles "ANY var IN array SATISFIES expr" (and EVERY, and ANY AND EVERY)
    void QueryParser::anyEveryOp(slice op, Array::iterator& operands) {
        auto var = (string)requiredString(operands[0], "ANY/EVERY first parameter");
        require(isValidIdentifier(var),
                "ANY/EVERY first parameter must be an identifier; '%s' is not", var.c_str());
        require(_variables.count(var) == 0, "Variable '%s' is already in use", var.c_str());
        _variables.insert(var);

        const Value *arraySource = operands[1];
        auto predicate = requiredArray(operands[2], "ANY/EVERY third parameter");

        bool every = !op.caseEquivalent("ANY"_sl);
        bool anyAndEvery = op.caseEquivalent("ANY AND EVERY"_sl);

        if (op.caseEquivalent("ANY"_sl) && predicate->count() == 3
                                        && predicate->get(0)->asString() == "="_sl
                                        && propertyFromNode(predicate->get(1), '?') == var) {
            // If predicate is `var = value`, generate `fl_contains(array, value)` instead
            writeFunctionGetter(kContainsFnName, arraySource, predicate->get(2));
            return;
        }

        if (anyAndEvery) {
            _sql << '(';
            writeFunctionGetter(kCountFnName, arraySource);
            _sql << " > 0 AND ";
        }

        if (every)
            _sql << "NOT ";
        _sql << "EXISTS (SELECT 1 FROM ";
        writeEachExpression(arraySource);
        _sql << " AS _" << var << " WHERE ";
        if (every)
            _sql << "NOT (";
        parseNode(predicate);
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


    // Handles object (dict) property accessors, e.g. ["_.", [..., "prop"] --> fl_property(..., "prop")
    void QueryParser::objectPropertyOp(slice op, Array::iterator& operands) {
        _sql << kNestedValueFnName << '(';
        _context.push_back(&kArgListOperation);     // prevents extra parens around operands
        parseNode(operands[0]);
        _context.pop_back();

        slice path;
        if (op.size == 2) {
            require(operands.count() == 2, "Missing object-property path parameter");
            path = requiredString(operands[1], "object property path");
        } else {
            require(operands.count() == 1, "Excess object-property parameter");
            path = op;
            path.moveStart(2);
        }

        _sql << ", ";
        writeSQLString(path);
        _sql << ")";
    }


    void QueryParser::blobOp(slice op, Array::iterator& operands) {
        writePropertyGetter(kBlobFnName, propertyFromString(requiredString(operands[0], "blob path")));
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
            if (hasPrefix(property, "$"))
                property.insert(0, 1, '\\');
        }

        require(isValidIdentifier(var), "Invalid variable name '%.*s'", SPLAT(op));
        require(_variables.count(var) > 0, "No such variable '%.*s'", SPLAT(op));

        // Now generate the function call:
        if (property.empty()) {
            _sql << '_' << var << ".value";
        } else {
            _sql << kNestedValueFnName << "(_" << var << ".body, ";
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
            QueryParser nested(this);
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

        if (op.hasPrefix('.')) {
            writePropertyGetter(kValueFnName, propertyFromString(op));
        } else if (op.hasPrefix("_."_sl)) {
            objectPropertyOp(op, operands);
        } else if (op.hasPrefix('$')) {
            parameterOp(op, operands);
        } else if (op.hasPrefix('?')) {
            variableOp(op, operands);
        } else if (op.hasSuffix("()"_sl)) {
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
            auto i = _indexJoinTables.find(fts);
            if (i == _indexJoinTables.end())
                fail("rank() can only be called on FTS indexes");
            _sql << "rank(matchinfo(" << i->second << ".\"" << i->first << "\"))";
            return;
        }

        // Special case: "prediction()" may be indexed:
#ifdef COUCHBASE_ENTERPRISE
        if (op.caseEquivalent(kPredictionFnName) && writeIndexedPrediction((const Array*)_curNode))
            return;
#endif

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

    void QueryParser::writeDictLiteral(const fleece::impl::Dict *dict) {
        _context.push_back(&kArgListOperation);
        _sql << kDictFnName << '(';
        int n = 0;
        for (Dict::iterator i(dict); i; ++i) {
            if (n++ > 0)
                _sql << ", ";
            writeSQLString(i.keyString());
            _sql << ", ";
            parseNode(i.value());
        }
        _sql << ')';
        _context.pop_back();
    }


#pragma mark - PROPERTIES:


    namespace qp {
        string propertyFromString(slice str) {
            require(str.hasPrefix('.'),
                    "Invalid property name '%.*s'; must start with '.'", SPLAT(str));
            str.moveStart(1);
            auto property = str.asString();
            if (str.hasPrefix('$'))
                property.insert(0, 1, '\\');
            return property;
        }


        // Concatenates property operands to produce the property path string
        string propertyFromOperands(Array::iterator &operands, bool skipDotPrefix) {
            stringstream pathStr;
            int n = 0;
            for (auto &i = operands; i; ++i,++n) {
                auto arr = i.value()->asArray();
                if (arr) {
                    require(n > 0, "Property path can't start with an array index");
                    require(arr->count() == 1, "Property array index must have exactly one item");
                    require(arr->get(0)->isInteger(), "Property array index must be an integer");
                    Path::writeIndex(pathStr, (int)arr->get(0)->asInt());
                } else {
                    slice name = i.value()->asString();
                    require(name, "Invalid JSON value in property path");
                    if (skipDotPrefix) {
                        name.moveStart(1);
                        pathStr.write((const char*)name.buf, name.size);
                    } else {
                        Path::writeProperty(pathStr, name, n==0);
                    }
                    require(name.size > 0, "Property name must not be empty");
                }
                skipDotPrefix = false;
            }
            return pathStr.str();
        }


        // Returns the property represented by a node, or "" if it's not a property node
        string propertyFromNode(const Value *node, char prefix) {
            Array::iterator i(node->asArray());
            if (i.count() >= 1) {
                auto op = i[0]->asString();
                if (op.hasPrefix(prefix)) {
                    bool justDot = (op.size == 1);
                    if (justDot)
                        ++i;
                    return propertyFromOperands(i, !justDot);
                }
            }
            return "";              // not a valid property node
        }
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


    void QueryParser::writeFunctionGetter(slice fn, const Value *source, const Value *param) {
        string property = propertyFromNode(source);
        if (property.empty()) {
            _sql << fn << "(";
            parseNode(source);
            if (param) {
                _sql << ", null, ";
                parseNode(param);
            }
            _sql << ")";
        } else {
            writePropertyGetter(fn, property, param);
        }
    }


    void QueryParser::writeMetaProperty(slice fn, const string &tablePrefix, const char *property) {
        require(fn == kValueFnName, "can't use '_%s' in this context", property);
        _sql << tablePrefix << property;
    }


    // Writes a call to a Fleece SQL function, including the closing ")".
    void QueryParser::writePropertyGetter(slice fn, string property, const Value *param) {
        string alias, tablePrefix;
        if (_propertiesUseAliases) {
            // Interpret the first component of the property as a db alias:
            auto dot = property.find('.');
            string rest;
            if (dot == string::npos) {
                dot = property.size();
            } else {
                rest = property.substr(dot+1);
            }
            alias = property.substr(0, dot);

            // Make sure there isn't a bracket (array index) before the dot:
            auto bra = property.find('[');
            require(bra == string::npos || dot < bra,
                    "Missing database alias name in property '%s'", property.c_str());
            property = rest;
        } else {
            alias = _dbAlias;
        }
        if (!alias.empty())
            tablePrefix = quoteTableName(alias) + ".";

        auto iType = _aliases.find(alias);
        require(iType != _aliases.end(),
                "property '%s.%s' does not begin with a declared 'AS' alias",
                alias.c_str(), property.c_str());
        if (iType->second >= kUnnestVirtualTableAlias) {
            // The alias is to an UNNEST. This needs to be written specially:
            writeUnnestPropertyGetter(fn, property, alias, iType->second);
            return;
        }

        if (property == kDocIDProperty) {
            writeMetaProperty(fn, tablePrefix, "key");
        } else if (property == kSequenceProperty) {
            writeMetaProperty(fn, tablePrefix, "sequence");
        } else if (property == kExpirationProperty) {
            writeMetaProperty(fn, tablePrefix, "expiration");
            _checkedExpiration = true;
        } else if (property == kDeletedProperty) {
            require(fn == kValueFnName, "can't use '_deleted' in this context");
            writeDeletionTest(alias, true);
            _checkedDeleted = true;     // note that the query has tested _deleted
        } else {
            // It's more efficent to get the doc root with fl_root than with fl_value:
            if (property == "" && fn == kValueFnName)
                fn = kRootFnName;

            // Write the function call:
            _sql << fn << "(" << tablePrefix << _bodyColumnName;
            if(!property.empty()) {
                _sql << ", ";
                writeSQLString(_sql, slice(property));
            }
            if (param) {
                _sql << ", ";
                parseNode(param);
            }
            _sql << ")";
        }
    }


    void QueryParser::writeUnnestPropertyGetter(slice fn, const string &property,
                                                const string &alias, aliasType type)
    {
        require(fn == kValueFnName, "can't use an UNNEST alias in this context");
        require(property != kDocIDProperty && property != kSequenceProperty,
                "can't use '%s' on an UNNEST", property.c_str());
        string tablePrefix;
        if (_propertiesUseAliases)
            tablePrefix = quoteTableName(alias) + ".";

        if (type == kUnnestVirtualTableAlias) {
            if (property.empty()) {
                _sql << tablePrefix << "value";
            } else {
                _sql << kNestedValueFnName << "(" << tablePrefix << "body, ";
                writeSQLString(_sql, slice(property));
                _sql << ")";
            }
        } else {
            _sql << kUnnestedValueFnName << "(" << tablePrefix << "body";
            if (!property.empty()) {
                _sql << ", ";
                writeSQLString(_sql, slice(property));
            }
            _sql << ")";
        }
    }


    // Writes an 'fl_each()' call representing a virtual table for the array at the given property
    void QueryParser::writeEachExpression(const string &property) {
        require(!property.empty(), "array expressions only support a property as their source");
#if 0
        // Is the property an existing UNNEST alias?
        for (auto &elem : _unnestAliases) {
            if (elem.second == property) {
                _sql << property;
                return;
            }
        }

        auto i = _unnestAliases.find(property);
        if (i != _unnestAliases.end())
            _sql << i->second;                              // write existing table alias
        else
#endif
        writePropertyGetter(kEachFnName, property);     // write fl_each()
    }

    // Writes an 'fl_each()' call representing a virtual table for the array at the given property
    void QueryParser::writeEachExpression(const Value *propertyExpr) {
        writeFunctionGetter(kEachFnName, propertyExpr);
//        writeEachExpression(propertyFromNode(propertyExpr));
    }


    std::string QueryParser::expressionSQL(const fleece::impl::Value* expr) {
        reset();
        parseJustExpression(expr);
        return SQL();
    }


    std::string QueryParser::eachExpressionSQL(const fleece::impl::Value* arrayExpr) {
        reset();
        writeEachExpression(arrayExpr);
        return SQL();
    }


    // Given an index table name, returns its join alias. If `aliasPrefix` is given, it will add
    // a new alias if necessary, which will begin with that prefix.
    const string& QueryParser::indexJoinTableAlias(const string &tableName, const char *aliasPrefix) {
        auto i = _indexJoinTables.find(tableName);
        if (i == _indexJoinTables.end()) {
            if (!aliasPrefix) {
                static string kEmptyString;
                return kEmptyString;
            }
            string alias = aliasPrefix + to_string(_indexJoinTables.size() + 1);
            i = _indexJoinTables.insert({tableName, alias}).first;
        }
        return i->second;
    }


#pragma mark - FULL-TEXT-SEARCH:


    // Recursively looks for MATCH expressions and adds the properties being matched to
    // _indexJoinTables. Returns the number of expressions found.
    unsigned QueryParser::findFTSProperties(const Value *root) {
        return findNodes(root, "MATCH"_sl, 1, [this](const Array *match) {
            FTSJoinTableAlias(match->get(1), true); // add LHS
        });
    }

    // Returns the FTS table name given the LHS of a MATCH expression.
    string QueryParser::FTSTableName(const Value *key) const {
        string ftsName( requiredString(key, "left-hand side of MATCH expression") );
        require(!ftsName.empty() && ftsName.find('"') == string::npos,
                "FTS index name may not contain double-quotes nor be empty");
        return _delegate.FTSTableName(ftsName);
    }

    // Returns or creates the FTS join alias given the LHS of a MATCH expression.
    const string& QueryParser::FTSJoinTableAlias(const Value *matchLHS, bool canAdd) {
        auto tableName = FTSTableName(matchLHS);
        const string &alias = indexJoinTableAlias(tableName);
        if (!canAdd || !alias.empty())
            return alias;
        _ftsTables.push_back(tableName);
        return indexJoinTableAlias(tableName, "fts");
    }


    // Returns the column name of an FTS table to use for a MATCH expression.
    string QueryParser::FTSColumnName(const Value *expression) {
        slice op = requiredArray(expression, "FTS index expression")->get(0)->asString();
        require(op.hasPrefix('.'), "FTS index expression must be a property");
        string property = propertyFromNode(expression);
        require(!property.empty(), "invalid property expression");
        return property;
    }



#pragma mark - UNNEST QUERY:


    // Constructs a unique identifier of an expression, from a digest of its JSON.
    string QueryParser::expressionIdentifier(const Array *expression, unsigned maxItems) const {
        require(expression, "Invalid expression to index");
        uint8_t digest[20];
        sha1Context ctx;
        sha1_begin(&ctx);
        unsigned item = 0;
        for (Array::iterator i(expression); i; ++i) {
            if (maxItems > 0 && ++item > maxItems)
                break;
            alloc_slice json = i.value()->toJSON(true);
            if (_propertiesUseAliases) {
                // Strip ".doc" from property paths if necessary:
                string s = json.asString();
                replace(s, "[\"." + _dbAlias + ".", "[\".");
                sha1_add(&ctx, s.data(), s.size());
            } else {
                sha1_add(&ctx, json.buf, json.size);
            }
        }
        sha1_end(&ctx, &digest);
        return slice(&digest, sizeof(digest)).base64String();
    }


    // Returns the index table name for an unnested array property.
    string QueryParser::unnestedTableName(const Value *arrayExpr) const {
        string path = propertyFromNode(arrayExpr);
        if (!path.empty()) {
            // It's a property path
            require(path.find('"') == string::npos,
                    "invalid property path for array index");
            if (_propertiesUseAliases) {
                string dbAliasPrefix = _dbAlias + ".";
                if (hasPrefix(path, dbAliasPrefix))
                    path = path.substr(dbAliasPrefix.size());
            }
        } else {
            // It's some other expression; make a unique digest of it:
            path = expressionIdentifier(arrayExpr->asArray());
        }
        return _delegate.unnestedTableName(path);
    }


#pragma mark - PREDICTIVE QUERY:


#ifndef COUCHBASE_ENTERPRISE
    void QueryParser::findPredictionCalls(const Value *root) {
    }
#endif

}
