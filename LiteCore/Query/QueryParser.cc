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
#include "NumConversion.hh"
#include <algorithm>
#include <unordered_set>

using namespace std;
using namespace fleece;
using namespace fleece::impl;
using namespace litecore::qp;

namespace litecore {

#pragma mark - UTILITY FUNCTIONS:

    namespace qp {
        struct caseInsensitiveSlice {
            size_t operator() (pure_slice const& s) const {
                uint32_t h = 2166136261;
                for (size_t i = 0; i < s.size; i++) {
                    h = (h ^ tolower(s[i])) * 16777619;
	            }
                        
	            return h;
            }

            bool operator()(const slice& a, const slice& b) const {
                return a.caseEquivalent(b);
            }
        };

        using case_insensitive_set = std::unordered_set<slice, caseInsensitiveSlice, caseInsensitiveSlice>;

        bool isImplicitBool(const Value* op) {
            if(!op) {
                return false;
            }

            static const case_insensitive_set implicitBoolOps = {
                "!="_sl, "="_sl, ">"_sl, "<"_sl, ">="_sl, "<="_sl,
                "IS"_sl, "IS NOT"_sl, "NOT"_sl,
                "BETWEEN"_sl, "AND"_sl, "OR"_sl,
                "NOT IN"_sl, "EVERY"_sl, "ANY AND EVERY"_sl
            };

            return implicitBoolOps.count(op->asString()) > 0;
        }

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
            slice str = required(required(v, what)->asString(), what, "must be a string");
            require(str.size > 0, what, "must be non-empty");
            return str;
        }

        const Value* getCaseInsensitive(const Dict *dict, slice key) {
            for (Dict::iterator i(dict); i; ++i)
                if (i.key()->asString().caseEquivalent(key))
                    return i.value();
            return nullptr;
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

    static bool isValidAlias(const string& alias) {
        return alias.find('"') == string::npos && alias.find('\\') == string::npos;
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

    static alloc_slice escapedPath(slice inputPath) {
        Assert(inputPath.peekByte() == '$');
        alloc_slice escaped(inputPath.size + 1);
        memcpy((void *)escaped.buf, "\\", 1);
        inputPath.readInto(escaped.from(1));
        return escaped;
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
        _isAggregateQuery = _aggregatesOK = _propertiesUseSourcePrefix = _checkedExpiration = false;

        _aliases.insert({_dbAlias, kDBAlias});
    }


    static void handleFleeceException(const FleeceException &x) {
        switch (x.code) {
            case PathSyntaxError:   fail("Invalid property path: %s", x.what());
            case JSONError:         fail("JSON parse error: %s", x.what());
            default:                throw;
        }
    }


    void QueryParser::parseJSON(slice expressionJSON) {
        Retained<Doc> doc;
        try {
            doc = Doc::fromJSON(expressionJSON);
        } catch (const FleeceException &x) {handleFleeceException(x);}
        return parse(doc->root());
    }
    
    
    void QueryParser::parse(const Value *expression) {
        reset();
        try {
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
        } catch (const FleeceException &x) {handleFleeceException(x);}
    }


    void QueryParser::parseJustExpression(const Value *expression) {
        reset();
        try {
            parseNode(expression);
        } catch (const FleeceException &x) {handleFleeceException(x);}
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
        if (_propertiesUseSourcePrefix)
            defaultTablePrefix = quoteTableName(_dbAlias) + ".";

        auto startPosOfWhat = _sql.tellp();
        _1stCustomResultCol = 0;

        auto nCustomCol = writeSelectListClause(operands, "WHAT"_sl, "", true);
        if (nCustomCol == 0) {
            // If no return columns are specified, add the docID and sequence as defaults
            _sql << defaultTablePrefix << "key, " << defaultTablePrefix << "sequence";
            _columnTitles.push_back(string(kDocIDProperty));
            _columnTitles.push_back(string(kSequenceProperty));
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
            extra << _dbAlias << ".rowid";

            // Write columns for the FTS match offsets (in order of appearance of the MATCH expressions)
            for (string &ftsTable : _ftsTables) {
                const string &alias = _indexJoinTables[ftsTable];
                extra << ", offsets(" << alias << ".\"" << ftsTable << "\")";
            }
            extra << ", ";
            string str = _sql.str();
            str.insert((string::size_type)startPosOfWhat, extra.str());
            _sql.str(str);
            _sql.seekp(0, stringstream::end);
            _1stCustomResultCol += 1U + narrow_cast<unsigned int>(_ftsTables.size());
        }

        // ORDER_BY clause:
        writeSelectListClause(operands, "ORDER_BY"_sl, " ORDER BY ", true);

        // LIMIT, OFFSET clauses:
        if (!writeOrderOrLimitClause(operands, "LIMIT"_sl,  "LIMIT")) {
            if (getCaseInsensitive(operands, "OFFSET"_sl))
                _sql << " LIMIT -1";            // SQL does not allow OFFSET without LIMIT
        }
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
                                       const Array *whereClause,
                                       bool isUnnestedTable)
    {
        reset();
        try {
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
            if (whereClause && !isUnnestedTable)
                writeWhereClause(whereClause);
        } catch (const FleeceException &x) {handleFleeceException(x);}
    }


    bool QueryParser::writeOrderOrLimitClause(const Dict *operands,
                                              slice jsonKey,
                                              const char *sqlKeyword) {
        auto value = getCaseInsensitive(operands, jsonKey);
        if (!value)
            return false;
        _sql << " " << sqlKeyword << " MAX(0, ";
        parseNode(value);
        _sql << ")";
        return true;
    }


#pragma mark - "FROM" / "JOIN" clauses:


    void QueryParser::addAlias(const string &alias, aliasType type) {
        require(isValidAlias(alias),
                "Invalid AS identifier '%s'", alias.c_str());
        require(_aliases.find(alias) == _aliases.end(),
                "duplicate AS identifier '%s'", alias.c_str());
        _aliases.insert({alias, type});
        if (type == kDBAlias)
            _dbAlias = alias;
    }


    void QueryParser::parseFromClause(const Value *from) {
        _aliases.clear();
        bool first = true;
        if (from) {
            for (Array::iterator i(requiredArray(from, "FROM value")); i; ++i) {
                if (first)
                    _propertiesUseSourcePrefix = true;
                auto entry = requiredDict(i.value(), "FROM item");
                string alias = requiredString(getCaseInsensitive(entry, "AS"_sl),
                                              "AS in FROM item").asString();

                // Determine the alias type:
                auto unnest = getCaseInsensitive(entry, "UNNEST"_sl);
                auto on = getCaseInsensitive(entry, "ON"_sl);

                aliasType type;
                if (first) {
                    require(!on && !unnest, "first FROM item cannot have an ON or UNNEST clause");
                    type = kDBAlias;
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
                addAlias(alias, type);
                first = false;
            }
        }
        if (first)
            addAlias(kDefaultTableAlias, kDBAlias);
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

                        _sql << " " << kJoinTypeNames[ joinType ];
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
                    default:
                        Assert(false, "Impossible alias type");
                        break;
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
                _sql << node->toString();                
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
        if (_context.back() == &kColumnListOperation) {
            writePropertyGetter(kValueFnName, Path(str));
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
        bool functionWantsCollation = _functionWantsCollation;
        _functionWantsCollation = false;

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

        if(functionWantsCollation) {
            if(n > 0) {
                _sql << ", ";
            }

            _sql << "'" << _collation.sqliteName() << "'";
        }
    }


    static string columnTitleFromProperty(const Path &property, bool useAlias) {
        if (property.empty())
            return "*";
        string first(property[0].keyStr());
        if (first[0] == '_') {
            return first.substr(1);         // meta property
        } else {
            return string(property[property.size()-1].keyStr());
        }
    }


    // Handles the WHAT clause (list of results)
    void QueryParser::resultOp(slice op, Array::iterator& operands) {
        int n = 0;
        unsigned anonCount = 0;
        for (auto &i = operands; i; ++i) {
            // Write the operation/delimiter between arguments
            if (n++ > 0)
                _sql << ", ";

            auto result = i.value();
            string title;
            Array::iterator expr(result->asArray());
            if (expr && expr[0]->asString().caseEquivalent("AS"_sl)) {
                // Handle 'AS':
                require(expr.count() == 3, "'AS' must have two operands");
                title = string(requiredString(expr[2], "'AS' alias"));

                result = expr[1];
                _sql << kResultFnName << "(";
                parseCollatableNode(result);
                _sql << ") AS \"" << title << '"';
                addAlias(title, kResultAlias);
            } else {
                _sql << (isImplicitBool(expr[0]) ? kBoolResultFnName : kResultFnName) << "(";
                if (result->type() == kString) {
                    // Convenience shortcut: interpret a string in a WHAT as a property path
                    writePropertyGetter(kValueFnName, Path(result->asString()));
                } else {
                    parseCollatableNode(result);
                }
                _sql << ")";

                // Come up with a column title if there is no 'AS':
                if (result->type() == kString) {
                    title = columnTitleFromProperty(Path(result->asString()), _propertiesUseSourcePrefix);
                } else if (result->type() == kArray && expr[0]->asString().hasPrefix('.')) {
                    title = columnTitleFromProperty(propertyFromNode(result), _propertiesUseSourcePrefix);
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


    // Handles "x || y", turning it into a call to the concat() function
    void QueryParser::concatOp(slice op, Array::iterator& operands) {
        functionOp("concat()"_sl, operands);
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

    void QueryParser::likeOp(slice op, Array::iterator& operands) {
        // Optimization: use SQLite's built-in LIKE function when possible, i.e. when the collation
        // in effect matches SQLite's BINARY collation. This allows the query optimizer to use the
        // "LIKE optimization", allowing an indexed prefix search, when the pattern is a literal or
        // parameter and doesn't begin with a wildcard. (CBL-890)
        // <https://sqlite.org/optoverview.html#like_opt>
        if (_collation.caseSensitive && _collation.diacriticSensitive && !_collation.unicodeAware) {
            parseCollatableNode(operands[0]);
            _sql << " LIKE ";
            parseCollatableNode(operands[1]);
            _sql << " ESCAPE '\\'";
        } else {
            // Otherwise invoke our custom `fl_like` function, which supports other collations:
            functionOp("fl_like()"_sl, operands);
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
                                        && string(propertyFromNode(predicate->get(1), '?')) == var) {
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

    bool QueryParser::optimizeMetaKeyExtraction(Array::iterator& operands) {
        // Handle Meta().id - N1QL
        // ["_.", ["meta" <db>], ".id"] - JSON

        const Array* metaop = operands[0]->asArray();
        if (metaop == nullptr || metaop->count() == 0 ||
            metaop->begin().value()->asString() != "meta"_sl) {
            return false;
        }
        slice dbAlias;
        if (metaop->count() > 1) {
            const Value* second = metaop->get(1);
            if (second->type() == kString) {
                dbAlias = second->asString();
            }
        }
        slice meta_key = operands[1]->asString();
        if (meta_key == nullptr) {
            return false;
        }
        if (meta_key[0] == '.') {
            meta_key.moveStart(1);
        }
        string dbAlias_s = dbAlias.asString();
        Path path {slice(dbAlias_s +".id")};
        const auto& dbIter = verifyDbAlias(path);
        require(dbAlias_s.empty() || dbAlias_s == dbIter->first,
                "database alias '%s' does not match a declared 'AS' alias", dbAlias_s.c_str());

        writeMetaPropertyGetter(meta_key, dbIter->first);
        return true;
    }


    // Handles object (dict) property accessors, e.g. ["_.", [...], "prop"] --> fl_nested_value(..., "prop")
    void QueryParser::objectPropertyOp(slice op, Array::iterator& operands) {
        auto nOperands = operands.count();
        
        if (nOperands == 2 && optimizeMetaKeyExtraction(operands)) {
            return;
        }
        
        _sql << kNestedValueFnName << '(';
        _context.push_back(&kArgListOperation);     // prevents extra parens around operands
        require(nOperands > 0, "Missing dictionary parameter for '%.*s'", SPLAT(op));
        parseNode(operands[0]);
        _context.pop_back();

        slice path;
        if (op.size == 2) {
            require(nOperands == 2, "Missing object-property path parameter");
            path = requiredString(operands[1], "object property path");
        } else {
            require(nOperands == 1, "Excess object-property parameter");
            path = op;
            path.moveStart(2);
        }

        _sql << ", ";
        writeSQLString(path);
        _sql << ")";
    }


    void QueryParser::blobOp(slice op, Array::iterator& operands) {
        writePropertyGetter(kBlobFnName, Path(requiredString(operands[0], "blob path")));
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
        Path path;
        if (op.size > 1) {
            op.moveStart(1);
            path += Path(op.asString());
        }
        if (operands.count() > 0) {
            path += propertyFromOperands(operands);
        }

        // Split the path into variable name and property:
        string var(path[0].keyStr());
        path.drop(1);

        require(isValidIdentifier(var), "Invalid variable name '%.*s'", SPLAT(op));
        require(_variables.count(var) > 0, "No such variable '%.*s'", SPLAT(op));

        // Now generate the function call:
        if (path.empty()) {
            _sql << '_' << var << ".value";
        } else {
            _sql << kNestedValueFnName << "(_" << var << ".body, ";
            writeSQLString(_sql, slice(string(path)));
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

    namespace {
        slice const kMetaKeys[] = {
            "id"_sl,
            "sequence"_sl,
            "deleted"_sl,
            "expiration"_sl,
            "revisionID"_sl
        };
        enum {
            mkId,
            mkSequence,
            mkDeleted,
            mkExpiration,
            mkRevisionId,
            mkCount
        };
        static_assert(sizeof(kMetaKeys) / sizeof(kMetaKeys[0]) == mkCount);
    }

    // Handles ["meta", dbAlias_optional]
    void QueryParser::metaOp(slice op, Array::iterator& operands) {
        // Pre-conditions: op == "meta"
        //                 operands.size() == 0 || operands[0]->type() == kString (dbAlias)

        string arg;
        if (operands.count() > 0 && operands[0]->type() == kString) {
            arg = operands[0]->asString();
        }
        
        Path path {slice(arg+".id")};
        const auto& dbIter = verifyDbAlias(path);
        require(arg.empty() || arg == dbIter->first,
                "database alias '%s' does not match a declared 'AS' alias", arg.c_str());

        _sql << kDictFnName << '(';
        bool first = true;
        for (slice k: kMetaKeys) {
            if (!first) {
                _sql << ", ";
            } else {
                first = false;
            }
            writeSQLString(k);
            _sql << ", ";
            writeMetaPropertyGetter(k, dbIter->first);
        }
        _sql << ')';
    }

    void QueryParser::writeMetaPropertyGetter(slice metaKey, const string& dbAlias) {
        string tablePrefix;
        if (!dbAlias.empty()) {
            tablePrefix = quoteTableName(dbAlias) + ".";
        }

        auto b = &kMetaKeys[0];
        auto it = find_if(b, b + mkCount, [metaKey](auto & p) {
            return p == metaKey;
        });
        require(it != b + mkCount, "'%s' is not a valid Meta key", metaKey.asString().c_str());

        switch (int i = (int)(it - b)) {
            case mkId:
                writeMetaProperty(kValueFnName, tablePrefix, "key");
                break;
            case mkDeleted:
                writeDeletionTest(dbAlias, true);
                _checkedDeleted = true;     // note that the query has tested _deleted
                break;
            case mkRevisionId:
                _sql << kVersionFnName << "(" << tablePrefix << "version" << ")";
                break;
            case mkSequence:
                writeMetaProperty(kValueFnName, tablePrefix, kMetaKeys[i].cString());
                _checkedExpiration = true;
                break;
            case mkExpiration:
                writeMetaProperty(kValueFnName, tablePrefix, kMetaKeys[i].cString());
                break;
            default:
                Assert(false, "Internal logic error");
                break;
        };
    }


    // Handles unrecognized operators, based on prefix ('.', '$', '?') or suffix ('()').
    void QueryParser::fallbackOp(slice op, Array::iterator& operands) {
        // Put the actual op into the context instead of a null
        auto operation = *_context.back();
        operation.op = op;
        _context.back() = &operation;

        if (op.hasPrefix('.')) {
            op.moveStart(1); // Skip initial .
            if(op.peekByte() == '$') {
                alloc_slice escaped = escapedPath(op);
                writePropertyGetter(kValueFnName, Path(escaped));
            } else {
                writePropertyGetter(kValueFnName, Path(op));
            }
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

        if (spec->name == "match"_sl) {
            matchOp(op, operands);
            return;
        }

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

        if(!_collationUsed && spec->wants_collation) {
            _collationUsed = true;
            _functionWantsCollation = true;
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
        // Concatenates property operands to produce the property path string
        Path propertyFromOperands(Array::iterator &operands, bool firstIsEncoded) {
            Path path;
            int n = 0;
            for (auto &i = operands; i; ++i,++n) {
                auto arr = i.value()->asArray();
                if (arr) {
                    require(n > 0, "Property path can't start with an array index");
                    require(arr->count() == 1, "Property array index must have exactly one item");
                    require(arr->get(0)->isInteger(), "Property array index must be an integer");
                    path.addIndex( (int)arr->get(0)->asInt() );
                } else {
                    slice name = i.value()->asString();
                    require(name, "Invalid JSON value in property path");
                    if (firstIsEncoded) {
                        name.moveStart(1);              // skip '.', '?', '$'
                        if(name.peekByte() == '$') {
                            alloc_slice escaped = escapedPath(name);
                            path.addComponents(escaped);
                        } else {
                            path.addComponents(name);
                        }
                    } else {
                        path.addProperty(name);
                    }
                }
                firstIsEncoded = false;
            }
            return path;
        }


        // Returns the property represented by a node, or "" if it's not a property node
        Path propertyFromNode(const Value *node, char prefix) {
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
            return Path();              // not a valid property node
        }
    }


    // If the first operand is a property operation, writes it using the given SQL function name
    // and returns true; else returns false.
    bool QueryParser::writeNestedPropertyOpIfAny(slice fnName, Array::iterator &operands) {
        if (operands.count() == 0 )
            return false;
        Path property = propertyFromNode(operands[0]);
        if (property.empty())
            return false;
        writePropertyGetter(fnName, move(property));
        return true;
    }


    void QueryParser::writeFunctionGetter(slice fn, const Value *source, const Value *param) {
        Path property = propertyFromNode(source);
        if (property.empty()) {
            _sql << fn << "(";
            parseNode(source);
            if (param) {
                _sql << ", null, ";
                parseNode(param);
            }
            _sql << ")";
        } else {
            writePropertyGetter(fn, move(property), param);
        }
    }


    void QueryParser::writeMetaProperty(slice fn, const string &tablePrefix, const char *property) {
        require(fn == kValueFnName, "can't use '_%s' in this context", property);
        _sql << tablePrefix << property;
    }

    // Return the iterator to _aliases based on the property.
    // Post-condition: iterator != _aliases.end()
    std::map<std::string, QueryParser::aliasType>::const_iterator
    QueryParser::verifyDbAlias(fleece::impl::Path &property) {
        string alias;
        auto iType = _aliases.end();
        if(!property.empty()) {
            // Check for result alias before 'alias' gets reassigned below
            alias = string(property[0].keyStr());
            iType = _aliases.find(alias);
        }

        bool hasMultiDbAliases = false;
        if (_aliases.size() > 1) {
            int cnt = 0;
            for (auto it = this->_aliases.begin(); it != this->_aliases.end(); ++it) {
                if (it->second != kResultAlias) {
                    if (++cnt == 2) {
                        hasMultiDbAliases = true;
                        break;
                    }
                }
            }
        }
        if (_propertiesUseSourcePrefix && !property.empty()) {
            // Interpret the first component of the property as a db alias:
            require(property[0].isKey(), "Property path can't start with array index");
            if (hasMultiDbAliases || alias == _dbAlias) {
                // With join (size > 1), properties must start with a keyspace alias to avoid ambiguity.
                // Otherwise, we assume property[0] to be the alias if it coincides with the unique one.
                // Otherwise, we consider that the property path starts in the document and, hence, do not drop.
                property.drop(1);
            } else {
                alias = _dbAlias;
            }
        } else {
            alias = _dbAlias;
        }

        if(iType == _aliases.end()) {
            iType = _aliases.find(alias);
        }

        require(iType != _aliases.end(),
                "property '%s.%s' does not begin with a declared 'AS' alias",
                alias.c_str(), string(property).c_str());

        return iType;
    }

    // Writes a call to a Fleece SQL function, including the closing ")".
    void QueryParser::writePropertyGetter(slice fn, Path &&property, const Value *param) {
        auto &&iType = verifyDbAlias(property);
        string alias = iType->first;
        string tablePrefix = alias.empty() ? "" : quoteTableName(alias) + ".";
        
        if (iType->second >= kUnnestVirtualTableAlias) {
            // The alias is to an UNNEST. This needs to be written specially:
            writeUnnestPropertyGetter(fn, property, alias, iType->second);
            return;
        }

        if(iType->second == kResultAlias && property[0].keyStr().asString() == iType->first) {
            // If the property in question is identified as an alias, emit that instead of
            // a standard getter since otherwise it will probably be wrong (i.e. doc["alias"]
            // vs alias -> doc["path"]["to"]["value"])
            if(property.size() == 1) {
                // Simple case, the alias is being used as-is
                _sql << '"' << iType->first << '"';
                return;
            }

            // More complicated case.  A subpath of an alias that points to
            // a collection type (e.g. alias = {"foo": "bar"}, and want to
            // ORDER BY alias.foo
            property.drop(1);
            _sql << kNestedValueFnName << "(\"" << iType->first << "\", '" << string(property) << "')";
            return;
        } 
        
        if (property.size() == 1) {
            // Check if this is a document metadata property:
            slice meta = property[0].keyStr();
            if (meta == kDocIDProperty) {
                writeMetaProperty(fn, tablePrefix, "key");
                return;
            } else if (meta == kSequenceProperty) {
                writeMetaProperty(fn, tablePrefix, "sequence");
                return;
            } else if (meta == kExpirationProperty) {
                writeMetaProperty(fn, tablePrefix, "expiration");
                _checkedExpiration = true;
                return;
            } else if (meta == kDeletedProperty) {
                require(fn == kValueFnName, "can't use '_deleted' in this context");
                writeDeletionTest(alias, true);
                _checkedDeleted = true;     // note that the query has tested _deleted
                return;
            } else if (meta == kRevIDProperty) {
                _sql << kVersionFnName << "(" << tablePrefix << "version" << ")";
                return;
            }
        }

        // It's more efficent to get the doc root with fl_root than with fl_value:
        if (property.empty() && fn == kValueFnName)
            fn = kRootFnName;

        // Write the function call:
        _sql << fn << "(" << tablePrefix << _bodyColumnName;
        if(!property.empty()) {
            _sql << ", ";
            writeSQLString(_sql, string(property));
        }
        if (param) {
            _sql << ", ";
            parseNode(param);
        }
        _sql << ")";
    }


    void QueryParser::writeUnnestPropertyGetter(slice fn, Path &property,
                                                const string &alias, aliasType type)
    {
        require(fn == kValueFnName, "can't use an UNNEST alias in this context");
        string spec(property);
        require(slice(spec) != kDocIDProperty && slice(spec) != kSequenceProperty,
                "can't use '%s' on an UNNEST", spec.c_str());
        string tablePrefix;
        if (_propertiesUseSourcePrefix)
            tablePrefix = quoteTableName(alias) + ".";

        if (type == kUnnestVirtualTableAlias) {
            if (property.empty()) {
                _sql << tablePrefix << "value";
            } else {
                _sql << kNestedValueFnName << "(" << tablePrefix << "body, ";
                writeSQLString(_sql, slice(spec));
                _sql << ")";
            }
        } else {
            _sql << kUnnestedValueFnName << "(" << tablePrefix << "body";
            if (!property.empty()) {
                _sql << ", ";
                writeSQLString(_sql, slice(spec));
            }
            _sql << ")";
        }
    }


    // Writes an 'fl_each()' call representing a virtual table for the array at the given property
    void QueryParser::writeEachExpression(Path &&property) {
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
        writePropertyGetter(kEachFnName, move(property));     // write fl_each()
    }

    // Writes an 'fl_each()' call representing a virtual table for the array at the given property
    void QueryParser::writeEachExpression(const Value *propertyExpr) {
        writeFunctionGetter(kEachFnName, propertyExpr);
    }


    std::string QueryParser::expressionSQL(const fleece::impl::Value* expr) {
        reset();
        parseJustExpression(expr);
        return SQL();
    }

    std::string QueryParser::whereClauseSQL(const fleece::impl::Value* arrayExpr,
                                            string_view dbAlias)
    {
        reset();
        if (!dbAlias.empty())
            addAlias(string(dbAlias), kDBAlias);
        writeWhereClause(arrayExpr);
        string sql = SQL();
        if (sql[0] == ' ')
            sql.erase(sql.begin(), sql.begin() + 1);
        return sql;
    }

    std::string QueryParser::eachExpressionSQL(const fleece::impl::Value* arrayExpr) {
        reset();
        writeEachExpression(arrayExpr);
        return SQL();
    }

    std::string QueryParser::FTSExpressionSQL(const fleece::impl::Value* ftsExpr) {
        reset();
        writeFunctionGetter(kFTSValueFnName, ftsExpr);
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
        return findNodes(root, "MATCH()"_sl, 1, [this](const Array *match) {
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
        string property(propertyFromNode(expression));
        require(!property.empty(), "invalid property expression");
        return property;
    }



#pragma mark - UNNEST QUERY:


    // Constructs a unique identifier of an expression, from a digest of its JSON.
    string QueryParser::expressionIdentifier(const Array *expression, unsigned maxItems) const {
        require(expression, "Invalid expression to index");
        SHA1Builder ctx;
        unsigned item = 0;
        for (Array::iterator i(expression); i; ++i) {
            if (maxItems > 0 && ++item > maxItems)
                break;
            alloc_slice json = i.value()->toJSON(true);
            if (_propertiesUseSourcePrefix) {
                // Strip ".doc" from property paths if necessary:
                string s = json.asString();
                replace(s, "[\"." + _dbAlias + ".", "[\".");
                ctx << slice(s);
            } else {
                ctx << json;
            }
        }
        return slice(ctx.finish()).base64String();
    }


    // Returns the index table name for an unnested array property.
    string QueryParser::unnestedTableName(const Value *arrayExpr) const {
        string path(propertyFromNode(arrayExpr));
        if (!path.empty()) {
            // It's a property path
            require(path.find('"') == string::npos,
                    "invalid property path for array index");
            if (_propertiesUseSourcePrefix) {
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
