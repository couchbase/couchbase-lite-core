//
// QueryParser.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema

#include "QueryParser.hh"
#include "QueryParser+Private.hh"
#include "QueryParserTables.hh"
#include "Record.hh"
#include "DataFile.hh"
#include "Error.hh"
#include "Doc.hh"
#include "MutableDict.hh"
#include "DeepIterator.hh"
#include "Path.hh"
#include "Logging.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"
#include "SQLUtil.hh"
#include "NumConversion.hh"
#include "slice_stream.hh"
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
            size_t operator()(pure_slice const& s) const {
                uint32_t h = 2166136261;
                for ( size_t i = 0; i < s.size; i++ ) { h = (h ^ tolower(s[i])) * 16777619; }

                return h;
            }

            bool operator()(const slice& a, const slice& b) const { return a.caseEquivalent(b); }
        };

        using case_insensitive_set = std::unordered_set<slice, caseInsensitiveSlice, caseInsensitiveSlice>;

        bool isImplicitBool(const Value* op) {
            if ( !op ) { return false; }

            static const case_insensitive_set implicitBoolOps = {
                    "!="_sl,  "="_sl,       ">"_sl,   "<"_sl,  ">="_sl,     "<="_sl,    "IS"_sl,           "IS NOT"_sl,
                    "NOT"_sl, "BETWEEN"_sl, "AND"_sl, "OR"_sl, "NOT IN"_sl, "EVERY"_sl, "ANY AND EVERY"_sl};

            return implicitBoolOps.count(op->asString()) > 0;
        }

        void fail(const char* format, ...) {
            va_list args;
            va_start(args, format);
            string message = vformat(format, args);
            va_end(args);

            Warn("Invalid LiteCore query: %s", message.c_str());
            throw error(error::LiteCore, error::InvalidQuery, message);
        }

        const Array* requiredArray(const Value* v, const char* what) {
            return required(required(v, what)->asArray(), what, "must be an array");
        }

        const Dict* requiredDict(const Value* v, const char* what) {
            return required(required(v, what)->asDict(), what, "must be a dictionary");
        }

        slice requiredString(const Value* v, const char* what) {
            slice str = required(required(v, what)->asString(), what, "must be a string");
            require(str.size > 0, "%s must be non-empty", what);
            return str;
        }

        slice optionalString(const Value* v, const char* what) {
            slice str;
            if ( v ) {
                str = required(v->asString(), what, "must be a string");
                require(str.size > 0, "%s must be non-empty", what);
            }
            return str;
        }

        const Value* getCaseInsensitive(const Dict* dict, slice key) {
            for ( Dict::iterator i(dict); i; ++i )
                if ( i.key()->asString().caseEquivalent(key) ) return i.value();
            return nullptr;
        }

        unsigned findNodes(const Value* root, slice op, unsigned argCount, function_ref<void(const Array*)> callback) {
            unsigned n = 0;
            for ( DeepIterator di(root); di; ++di ) {
                auto operation = di.value()->asArray();
                if ( operation && operation->count() > argCount && operation->get(0)->asString().caseEquivalent(op) ) {
                    callback(operation);
                    ++n;
                }
            }
            return n;
        }
    }  // namespace qp

    static bool isValidAlias(const string& alias) { return !slice(alias).findAnyByteOf("'\":"); }

    static string quotedIdentifierString(slice str) {
        if ( isValidIdentifier(str) ) {
            return string(str);
        } else {
            stringstream s;
            s << sqlIdentifier(str);
            return s.str();
        }
    }

    static alloc_slice escapedPath(slice inputPath) {
        slice_istream in(inputPath);
        Assert(in.peekByte() == '$');
        alloc_slice escaped(in.size + 1);
        auto        dst = (char*)escaped.buf;
        dst[0]          = '\\';
        in.readAll(dst + 1, escaped.size - 1);
        return escaped;
    }

#pragma mark - QUERY PARSER TOP LEVEL:

    void QueryParser::reset() {
        _sql.str(string());
        _context.clear();
        _context.push_back(&kOuterOperation);
        _parameters.clear();
        _variables.clear();
        _kvTables.clear();
        _ftsTables.clear();
        _indexJoinTables.clear();
        _aliases.clear();
        _dbAlias.clear();
        _columnTitles.clear();
        _1stCustomResultCol = 0;
        _isAggregateQuery = _aggregatesOK = _propertiesUseSourcePrefix = _checkedExpiration = false;
    }

    static void handleFleeceException(const FleeceException& x) {
        switch ( x.code ) {
            case PathSyntaxError:
                fail("Invalid property path: %s", x.what());
            case JSONError:
                fail("JSON parse error: %s", x.what());
            default:
                throw;
        }
    }

    void QueryParser::parseJSON(slice expressionJSON) {
        Retained<Doc> doc;
        try {
            doc = Doc::fromJSON(expressionJSON);
        } catch ( const FleeceException& x ) { handleFleeceException(x); }
        return parse(doc->root());
    }

    void QueryParser::parse(const Value* expression) {
        reset();
        try {
            if ( expression->asDict() ) {
                // Given a dict; assume it's the operands of a SELECT:
                writeSelect(expression->asDict());
            } else {
                const Array* a = expression->asArray();
                if ( a && a->count() > 0 && a->get(0)->asString() == "SELECT"_sl ) {
                    // Given an entire SELECT statement:
                    parseNode(expression);
                } else {
                    // Given some other expression; treat it as a WHERE clause of an implicit SELECT:
                    Retained<MutableDict> select = MutableDict::newDict();
                    select->set("WHERE", expression);
                    writeSelect(select);
                }
            }
        } catch ( const FleeceException& x ) { handleFleeceException(x); }
    }

    void QueryParser::parseJustExpression(const Value* expression) {
        reset();
        addDefaultAlias();
        try {
            parseNode(expression);
        } catch ( const FleeceException& x ) { handleFleeceException(x); }
    }

#pragma mark - SELECT STATEMENT

    void QueryParser::writeSelect(const Dict* operands) {
        // Find all the joins in the FROM clause first, to populate alias info. This has to be done
        // before writing the WHAT clause, because that will depend on the aliases.
        auto from = getCaseInsensitive(operands, "FROM"_sl);
        parseFromClause(from);

        // Check whether the query accesses deleted docs, since this affects what tables to use:
        lookForDeleted(operands);

        // Have to find all properties involved in MATCH before emitting the FROM clause:
        auto where = getCaseInsensitive(operands, "WHERE"_sl);
        if ( where ) {
            unsigned numMatches = findFTSProperties(where);
            require(numMatches <= _ftsTables.size(), "Sorry, multiple MATCHes of the same property are not allowed");
        }

        // Add the indexed prediction() calls to _indexJoinTables now
        findPredictionCalls(operands);

        _sql << "SELECT ";

        // DISTINCT:
        auto distinct    = getCaseInsensitive(operands, "DISTINCT"_sl);
        auto distinctVal = distinct && distinct->asBool();
        if ( distinctVal ) {
            _sql << "DISTINCT ";
            _isAggregateQuery = true;
        }

        // WHAT clause:
        string defaultTablePrefix;
        if ( _propertiesUseSourcePrefix ) defaultTablePrefix = quotedIdentifierString(_dbAlias) + ".";

        auto startPosOfWhat = _sql.tellp();
        _1stCustomResultCol = 0;

        auto nCustomCol = writeSelectListClause(operands, "WHAT"_sl, "", true);
        if ( nCustomCol == 0 ) {
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
        if ( grouped ) _isAggregateQuery = true;

        // HAVING clause:
        auto having = getCaseInsensitive(operands, "HAVING"_sl);
        if ( having ) {
            require(grouped, "HAVING requires GROUP_BY");
            _sql << " HAVING ";
            _aggregatesOK = true;
            parseNode(having);
            _aggregatesOK = false;
        }

        // Now go back and prepend some WHAT columns needed for FTS:
        if ( !_isAggregateQuery && !_ftsTables.empty() ) {
            stringstream extra;
            extra << quotedIdentifierString(_dbAlias) << ".rowid";

            // Write columns for the FTS match offsets (in order of appearance of the MATCH expressions)
            for ( string& ftsTable : _ftsTables ) {
                const string& alias = _indexJoinTables[ftsTable];
                extra << ", offsets(" << alias << "." << sqlIdentifier(ftsTable) << ")";
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
        if ( !writeOrderOrLimitClause(operands, "LIMIT"_sl, "LIMIT") ) {
            if ( getCaseInsensitive(operands, "OFFSET"_sl) )
                _sql << " LIMIT -1";  // SQL does not allow OFFSET without LIMIT
        }
        writeOrderOrLimitClause(operands, "OFFSET"_sl, "OFFSET");
    }

    // Writes a SELECT statement's 'WHAT', 'GROUP BY' or 'ORDER BY' clause:
    unsigned QueryParser::writeSelectListClause(const Dict* operands, slice key, const char* sql, bool aggregatesOK) {
        auto param = getCaseInsensitive(operands, key);
        if ( !param ) return 0;
        auto     list  = requiredArray(param, "WHAT / GROUP BY / ORDER BY parameter");
        unsigned count = list->count();
        if ( count == 0 ) return 0;

        _sql << sql;
        _context.push_back(&kExpressionListOperation);  // suppresses parens around arg list
        Array::iterator items(list);
        _aggregatesOK = aggregatesOK;
        if ( key == "WHAT"_sl ) handleOperation(&kResultListOperation, kResultListOperation.op, items);
        else
            writeColumnList(items);
        _aggregatesOK = false;
        _context.pop_back();
        return count;
    }

    namespace {
        bool canDefaultTableHaveDeleted() { return true; }

        bool needPatchDeleteFlag(const string& table, QueryParser::DeletionStatus delStatus) {
            auto pos = table.rfind('_');
            if ( pos == string::npos || table.substr(pos + 1) != "default" ) { return false; }
            // We only need to patch the delete flag for kLiveDocs, because the other two
            // DeletionStatus are deduced from the query itself, and they must explicitly
            // appear in the query expression.
            return canDefaultTableHaveDeleted() && delStatus == QueryParser::kLiveDocs;
        }
    }  // anonymous namespace

    void QueryParser::writeWhereClause(const Value* where) {
        auto& aliasInfo       = _aliases[_dbAlias];
        bool  patchDeleteFlag = needPatchDeleteFlag(aliasInfo.tableName, aliasInfo.delStatus);

        if ( !where && !patchDeleteFlag ) {
            // We don't have where and don't need patch the delete flag.
            return;
        }

        _checkedDeleted = false;
        if ( patchDeleteFlag ) { _sql << " WHERE "; }
        if ( where ) {
            if ( patchDeleteFlag ) {
                _sql << "(";
            } else {
                _sql << " WHERE ";
            }
            parseNode(where);
            if ( patchDeleteFlag ) { _sql << ")"; }
        }
        if ( !_checkedDeleted && patchDeleteFlag ) {
            if ( where ) { _sql << " AND "; }
            writeDeletionTest(_dbAlias);
        }
    }

    void QueryParser::writeCreateIndex(const string& indexName, const string& onTableName,
                                       Array::iterator& expressionsIter, const Array* whereClause,
                                       bool isUnnestedTable) {
        _defaultTableName = onTableName;
        reset();
        addDefaultAlias();
        try {
            if ( isUnnestedTable ) _aliases[_dbAlias] = {kUnnestTableAlias, onTableName};
            _sql << "CREATE INDEX " << sqlIdentifier(indexName) << " ON " << sqlIdentifier(onTableName) << " ";
            if ( expressionsIter.count() > 0 ) {
                writeColumnList(expressionsIter);
            } else {
                // No expressions; index the entire body (this is used with unnested/array tables):
                Assert(isUnnestedTable);
                _sql << '(' << kUnnestedValueFnName << "(" << _bodyColumnName << "))";
            }
            if ( whereClause && !isUnnestedTable ) writeWhereClause(whereClause);
        } catch ( const FleeceException& x ) { handleFleeceException(x); }
    }

    bool QueryParser::writeOrderOrLimitClause(const Dict* operands, slice jsonKey, const char* sqlKeyword) {
        auto value = getCaseInsensitive(operands, jsonKey);
        if ( !value ) return false;
        _sql << " " << sqlKeyword << " MAX(0, ";
        parseNode(value);
        _sql << ")";
        return true;
    }

#pragma mark - "FROM" / "JOIN" clauses:

    void QueryParser::addDefaultAlias() {
        DebugAssert(_aliases.empty());
        _aliases.insert({_dbAlias, {kDBAlias, _dbAlias, _defaultCollectionName, _defaultTableName}});
    }

    void QueryParser::addAlias(aliasInfo&& entry) {
        require(isValidAlias(entry.alias), "Invalid AS identifier '%s'", entry.alias.c_str());
        require(_aliases.find(entry.alias) == _aliases.end(), "duplicate collection alias '%s'", entry.alias.c_str());
        if ( entry.type == kDBAlias ) _dbAlias = entry.alias;
        _aliases.insert({entry.alias, std::move(entry)});
    }

    void QueryParser::addAlias(const string& alias, aliasType type, const string& tableName) {
        aliasInfo entry;
        entry.type       = type;
        entry.alias      = alias;
        entry.collection = _defaultCollectionName;
        entry.tableName  = tableName;
        addAlias(std::move(entry));
    }

    QueryParser::aliasInfo QueryParser::parseFromEntry(const Value* value) {
        auto      dict       = requiredDict(value, "FROM item");
        slice     collection = optionalString(getCaseInsensitive(dict, "COLLECTION"_sl), "COLLECTION in FROM item");
        slice     scope      = optionalString(getCaseInsensitive(dict, "SCOPE"_sl), "SCOPE in FROM item");
        aliasInfo from;
        from.type   = aliasType(-1);
        from.dict   = dict;
        from.alias  = string(optionalString(getCaseInsensitive(dict, "AS"_sl), "AS in FROM item"));
        from.on     = getCaseInsensitive(dict, "ON"_sl);
        from.unnest = getCaseInsensitive(dict, "UNNEST"_sl);
        if ( collection ) {
            if ( scope ) from.collection = string(scope) + '.';
            from.collection += string(collection);
            from.tableName = _delegate.collectionTableName(from.collection, kLiveDocs);
            require(_delegate.tableExists(from.tableName), "no such collection \"%s\"", from.collection.c_str());
        } else if ( scope ) {
            fail("SCOPE in FROM item requires a COLLECTION too");
        } else {
            from.collection = _defaultCollectionName;
            from.tableName  = _defaultTableName;
            DebugAssert(_delegate.tableExists(from.tableName));
        }
        if ( from.alias.empty() ) {
            if ( collection ) {
                from.alias = from.collection;
            } else {
                from.alias = _defaultCollectionName;
            }
        }
        from.alias = DataFile::unescapeCollectionName(from.alias);
        return from;
    }

    // Sanity-checks the FROM clause, and  populates _kvTables and _aliases based on it.
    void QueryParser::parseFromClause(const Value* from) {
        _aliases.clear();
        bool first = true;
        if ( from ) {
            for ( Array::iterator i(requiredArray(from, "FROM value")); i; ++i ) {
                if ( first ) _propertiesUseSourcePrefix = true;
                aliasInfo entry = parseFromEntry(i.value());
                if ( first ) {
                    require(!entry.on && !entry.unnest, "first FROM item cannot have an ON or UNNEST clause");
                    entry.type = kDBAlias;
                    _kvTables.insert(entry.tableName);
                    _defaultCollectionName = entry.collection;
                    _defaultTableName      = entry.tableName;
                } else if ( !entry.unnest ) {
                    entry.type = kJoinAlias;
                    _kvTables.insert(entry.tableName);
                } else {
                    require(!entry.on, "cannot use ON and UNNEST together");
                    string unnestTable = unnestedTableName(entry.unnest);
                    if ( _delegate.tableExists(unnestTable) ) entry.type = kUnnestTableAlias;
                    else
                        entry.type = kUnnestVirtualTableAlias;
                    entry.tableName = "";
                }
                addAlias(std::move(entry));
                first = false;
            }
        }
        if ( first ) {
            // Default alias if there is no FROM clause:
            addAlias(kDefaultTableAlias, kDBAlias, _defaultTableName);
            _kvTables.insert(_defaultTableName);
        }
    }

    static string aliasOfFromEntry(const Value* value, const string& defaultAlias) {
        // c.f. QueryParser::parseFromEntry
        auto   dict = requiredDict(value, "FROM item");
        string ret{optionalString(getCaseInsensitive(dict, "AS"_sl), "AS in FROM item")};
        if ( ret.empty() ) {
            ret = string{optionalString(getCaseInsensitive(dict, "COLLECTION"_sl), "COLLECTION in FROM item")};
            if ( ret.empty() ) {
                ret = defaultAlias;
            } else {
                if ( auto scope = optionalString(getCaseInsensitive(dict, "SCOPE"_sl), "SCOPE in FROM item");
                     !scope.empty() ) {
                    ret = string(scope) + "." + ret;
                }
            }
        }
        return DataFile::unescapeCollectionName(ret);
    }

    void QueryParser::writeFromClause(const Value* from) {
        auto fromArray = (const Array*)from;  // already type-checked by parseFromClause

        if ( fromArray && !fromArray->empty() ) {
            for ( Array::iterator i(fromArray); i; ++i ) {
                auto       fromAlias = aliasOfFromEntry(i.value(), _defaultCollectionName);
                aliasInfo& entry     = _aliases.find(fromAlias)->second;
                switch ( entry.type ) {
                    case kDBAlias:
                        {
                            // The first item is the database alias:
                            _sql << " FROM " << sqlIdentifier(entry.tableName) << " AS " << sqlIdentifier(entry.alias);
                            break;
                        }
                    case kUnnestVirtualTableAlias:
                        // UNNEST: Use fl_each() to make a virtual table:
                        _sql << " JOIN ";
                        writeEachExpression(entry.unnest);
                        _sql << " AS " << sqlIdentifier(entry.alias);
                        break;
                    case kUnnestTableAlias:
                        {
                            // UNNEST: Optimize query by using the unnest table as a join source:
                            string unnestTable = unnestedTableName(entry.unnest);
                            _sql << " JOIN " << sqlIdentifier(unnestTable) << " AS " << sqlIdentifier(entry.alias)
                                 << " ON " << sqlIdentifier(entry.alias) << ".docid=" << sqlIdentifier(_dbAlias)
                                 << ".rowid";
                            break;
                        }
                    case kJoinAlias:
                        {
                            // A join:
                            JoinType joinType    = kInner;
                            slice    joinTypeStr = optionalString(getCaseInsensitive(entry.dict, "JOIN"), "JOIN value");
                            if ( joinTypeStr ) {
                                joinType = JoinType(parseJoinType(joinTypeStr));
                                require(joinType != kInvalidJoin, "Unknown JOIN type '%.*s'", SPLAT(joinTypeStr));
                            }

                            if ( joinType == kCross ) {
                                require(!entry.on, "CROSS JOIN cannot accept an ON clause");
                            } else {
                                require(entry.on, "FROM item needs an ON clause to be a join");
                            }

                            _sql << " " << kJoinTypeNames[joinType] << " JOIN " << sqlIdentifier(entry.tableName)
                                 << " AS " << sqlIdentifier(entry.alias);
                            _checkedDeleted = false;
                            if ( entry.on ) {
                                _sql << " ON (";
                                parseNode(entry.on);
                                _sql << ")";
                            }
                            bool patchDeleteFlag = needPatchDeleteFlag(entry.tableName, entry.delStatus);
                            if ( !_checkedDeleted && patchDeleteFlag ) {
                                if ( entry.on ) {
                                    _sql << " AND ";
                                } else {
                                    _sql << " ON ";
                                }
                                writeDeletionTest(entry.alias);
                            }
                            break;
                        }
                    default:
                        Assert(false, "Impossible alias type");
                        break;
                }
            }
        } else {
            _sql << " FROM " << sqlIdentifier(_defaultTableName) << " AS " << sqlIdentifier(_dbAlias);
        }

        // Add joins to index tables (FTS, predictive):
        for ( auto& ftsTable : _indexJoinTables ) {
            auto& table = ftsTable.first;
            auto& alias = ftsTable.second;
            auto  idxAt = table.find(KeyStore::kIndexSeparator);
            // Encoded in the name of the index table is collection against which the index
            // is created. "docID" of this table is to match the "rowid" of the collection table.
            // The left-side of the separator is the original collection.
            // The Prediction method also uses a separate table; only the separator is different.
            if ( idxAt == string::npos ) { idxAt = table.find(KeyStore::kPredictSeparator); }
            string coAlias;
            auto [prefixBegin, prefixEnd] = _ftsTableAliases.equal_range(table);
            if ( idxAt != string::npos ) {
                string collTable = table.substr(0, idxAt);
                for ( auto iter = _aliases.begin(); iter != _aliases.end(); ++iter ) {
                    if ( iter->second.type == kResultAlias ) { continue; }
                    if ( iter->second.tableName == collTable ) { coAlias = iter->first; }
                    if ( prefixBegin != _ftsTableAliases.end()
                         && std::find_if(prefixBegin, prefixEnd, [iter](const auto& i) {
                                return i.second == iter->first;
                            }) != prefixEnd ) {
                        break;
                    }
                }
            }
            DebugAssert(!coAlias.empty());
            if ( coAlias.empty() ) {
                // Hack, to be fixed!
                Warn("The collecion is not specified. Hacked it to default.");
                coAlias = _dbAlias;
            }
            _sql << " JOIN " << sqlIdentifier(table) << " AS " << alias << " ON " << alias
                 << ".docid = " << sqlIdentifier(coAlias) << ".rowid";
        }
    }

    int /*JoinType*/ QueryParser::parseJoinType(slice str) {
        for ( int i = 0; kJoinTypeNames[i]; ++i ) {
            if ( str.caseEquivalent(slice(kJoinTypeNames[i])) ) return i;  // really returns JoinType
        }
        return kInvalidJoin;
    }

#pragma mark - PARSING THE "WHERE" CLAUSE:

    void QueryParser::parseNode(const Value* node) {
        _curNode = node;
        switch ( node->type() ) {
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
    void QueryParser::parseCollatableNode(const Value* node) {
        if ( _collationUsed ) {
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

    void QueryParser::writeCollation() { _sql << " COLLATE " << sqlIdentifier(_collation.sqliteName()); }

    void QueryParser::parseOpNode(const Array* node) {
        Array::iterator array(node);
        require(array.count() > 0, "Empty JSON array");
        slice op = requiredString(array[0], "operation");
        ++array;

        // Look up the handler:
        unsigned         nargs       = min(array.count(), 9u);
        bool             nameMatched = false;
        const Operation* def;
        for ( def = kOperationList; def->op; ++def ) {
            if ( op.caseEquivalent(def->op) ) {
                nameMatched = true;
                if ( nargs >= def->minArgs && nargs <= def->maxArgs ) break;
            }
        }
        if ( nameMatched && !def->op ) fail("Wrong number of arguments to %.*s", SPLAT(op));
        handleOperation(def, op, array);
    }

    // Invokes an Operation's handler. Pushes Operation on the stack and writes parens if needed
    void QueryParser::handleOperation(const Operation* op, slice actualOperator, Array::iterator& operands) {
        bool parenthesize = (op->precedence <= _context.back()->precedence);
        _context.push_back(op);
        if ( parenthesize ) _sql << '(';

        auto handler = op->handler;
        (this->*handler)(actualOperator, operands);

        if ( parenthesize ) _sql << ')';
        _context.pop_back();
    }

    // Handles a node that's a string. It's treated as a string literal, except in the context of
    // a column-list ('FROM', 'ORDER BY', creating index, etc.) where it's a property path.
    void QueryParser::parseStringLiteral(slice str) {
        if ( _context.back() == &kColumnListOperation ) {
            writePropertyGetter(kValueFnName, Path(str));
        } else {
            _sql << sqlString(str);
        }
    }

#pragma mark - DELETED-DOC HANDLING:

    // Returns true if the expression is the `meta()` property of the given alias.
    static bool isMetaProperty(const Array* meta, slice alias, bool uniqAlias) {
        if ( !meta || meta->empty() || !meta->get(0)->asString().caseEquivalent("META()") ) return false;
        if ( meta->count() == 1 ) return alias.empty() || uniqAlias;
        else
            return meta->get(1)->asString() == alias;
    }

    // Returns true if the expression is a reference to the "deleted" meta-property of an alias.
    // This can be appear either as a document property `_deleted`, or as a references to the
    // `deleted` property of the `meta()` function.
    static bool isDeletedPropertyRef(const Array* operation, slice alias, bool uniqAlias) {
        if ( operation && !operation->empty() ) {
            slice op = operation->get(0)->asString();
            if ( op.hasPrefix('.') ) {
                // `["._deleted"]` or `[".<alias>._deleted"]`
                if ( Path path = propertyFromNode(operation, '.'); path.size() == 1 || path.size() == 2 ) {
                    if ( path[path.size() - 1].keyStr() != "_deleted" ) return false;
                    if ( path.size() == 1 ) return alias.empty() || uniqAlias;
                    else
                        return path[0].keyStr() == alias;
                }
            } else if ( op == "_." && operation->count() == 3 ) {
                // `["._", ["META()", <alias>], "deleted"]`
                slice prop = operation->get(2)->asString();
                return (prop == "deleted" || prop == ".deleted")
                       && isMetaProperty(operation->get(1)->asArray(), alias, uniqAlias);
            }
        }
        return false;
    }

    static bool isDeletedPropertyRef(const Value* expr, slice alias, bool uniqAlias) {
        return expr && isDeletedPropertyRef(expr->asArray(), alias, uniqAlias);
    }

    static bool findDeletedPropertyRefs(const Dict* expr, slice alias, bool uniqAlias) {
        const Value* what = getCaseInsensitive(expr, "WHAT");
        for ( DeepIterator iter(expr); iter; ++iter ) {
            if ( auto operation = iter.value()->asArray(); operation && operation != what ) {
                if ( isDeletedPropertyRef(operation, alias, uniqAlias) ) return true;
                if ( isMetaProperty(operation, alias, uniqAlias) ) {
                    // Found a reference to `meta()`, but if it's being used to access a property
                    // other than `deleted`, ignore it:
                    slice prop;
                    if ( auto parent = iter.parent(); parent && parent != what ) {
                        if ( auto parOp = parent->asArray(); parOp && parOp->count() >= 3 ) {
                            if ( parOp->get(0)->asString() == "_." && parOp->get(1) == operation )
                                prop = parOp->get(2)->asString();
                        }
                    }
                    if ( !prop || prop == "deleted" || prop == ".deleted" ) return true;
                }
            }
        }
        return false;
    }

    // Returns `kDeletedDocs` if the expression only matches deleted documents,
    // 'kLiveTable' if it doesn't access the 'deleted' meta-property at all,
    static bool matchesOnlyDeletedDocs(const Value* expr, slice alias, bool uniqAlias) {
        if ( isDeletedPropertyRef(expr, alias, uniqAlias) ) return true;
        if ( auto operation = expr->asArray(); operation && operation->count() >= 2 ) {
            Array::iterator operands(operation);
            slice           op = operands->asString();
            ++operands;
            if ( op == "=" || op == "==" ) {
                // Match ["=", ["._deleted"], true] or ["=", true, ["._deleted"]]
                if ( operands.count() == 2 ) {
                    return (operands[0]->asBool() == true && isDeletedPropertyRef(operands[1], alias, uniqAlias))
                           || (operands[1]->asBool() == true && isDeletedPropertyRef(operands[0], alias, uniqAlias));
                }
            } else if ( op.caseEquivalent("AND") ) {
                // Match ["AND", ... ["._deleted"] ...]
                for ( ; operands; ++operands )
                    if ( matchesOnlyDeletedDocs(operands.value(), alias, uniqAlias) ) return true;
            }
        }
        return false;
    }

    void QueryParser::lookForDeleted(const Dict* select) {
        const Value*               where = getCaseInsensitive(select, "WHERE");
        vector<AliasMap::iterator> aliasIters;
        for ( auto iter = _aliases.begin(); iter != _aliases.end(); ++iter ) {
            if ( iter->second.type == kDBAlias || iter->second.type == kJoinAlias ) { aliasIters.push_back(iter); }
        }

        for ( auto iter : aliasIters ) {
            aliasInfo& info  = iter->second;
            slice      alias = info.alias;
            if ( info.type == kDBAlias && !_propertiesUseSourcePrefix ) alias = ""_sl;
            auto type = kLiveDocs;
            if ( findDeletedPropertyRefs(select, alias, aliasIters.size() == 1) ) {
                if ( where && matchesOnlyDeletedDocs(where, alias, aliasIters.size() == 1) ) {
                    type = kDeletedDocs;
                    LogDebug(QueryLog, "QueryParser: only matches deleted docs in '%.*s'", SPLAT(alias));
                } else {
                    type = kLiveAndDeletedDocs;
                    LogDebug(QueryLog, "QueryParser: May match live and deleted docs in '%.*s'", SPLAT(alias));
                }
            } else {
                LogDebug(QueryLog, "QueryParser: Doesn't access meta(%.*s).deleted", SPLAT(alias));
            }

            if ( type != info.delStatus ) {
                info.delStatus = type;
                Assert(!info.collection.empty());
                info.tableName = _delegate.collectionTableName(info.collection, info.delStatus);
                if ( info.type == kDBAlias ) {
                    _defaultCollectionName = info.collection;
                    _defaultTableName      = info.tableName;
                }
                // We altered the table, so re-check its existence.
                DebugAssert(_delegate.tableExists(info.tableName));
            }

            if ( !canDefaultTableHaveDeleted() ) { return; }

            if ( info.tableName == "kv_del_default" ) {
                // default collection is not completely separated, that is,
                // this table does not contain all the deleted docs. We need to use "all_default."
                info.delStatus = kLiveAndDeletedDocs;
                info.tableName = _delegate.collectionTableName(info.collection, info.delStatus);
                Assert(info.tableName == "all_default");
                if ( info.type == kDBAlias ) {
                    _defaultCollectionName = info.collection;
                    _defaultTableName      = info.tableName;
                }
                // We altered the table, so re-check its existence.
                DebugAssert(_delegate.tableExists(info.tableName));
            }
        }
    }

    void QueryParser::writeDeletionTest(const string& alias, bool isDeleted) {
        auto& aliasInfo       = _aliases[alias];
        bool  patchDeleteFlag = needPatchDeleteFlag(aliasInfo.tableName, aliasInfo.delStatus);

        if ( patchDeleteFlag ) {
            _sql << "(";
            if ( !alias.empty() ) _sql << sqlIdentifier(alias) << '.';
            _sql << "flags & " << (unsigned)DocumentFlags::kDeleted << (isDeleted ? " != 0)" : " = 0)");
            return;
        }

        switch ( _aliases[alias].delStatus ) {
            case kLiveDocs:
                _sql << "false";
                break;
            case kDeletedDocs:
                _sql << "true";
                break;
            case kLiveAndDeletedDocs:
                _sql << "(";
                if ( !alias.empty() ) _sql << sqlIdentifier(alias) << '.';
                _sql << "flags & " << (unsigned)DocumentFlags::kDeleted << " != 0)";
                break;
        }
    }

#pragma mark - OPERATION HANDLERS:

    // Handles prefix (unary) operators
    void QueryParser::prefixOp(slice op, Array::iterator& operands) {
        _sql << op;
        if ( isalpha(op[op.size - 1]) ) _sql << ' ';
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
        _functionWantsCollation     = false;

        if ( operands.count() >= 2 && operands[1]->type() == kNull ) {
            // Ugly special case where SQLite's semantics for 'IS [NOT]' don't match N1QL's (#410)
            if ( op.caseEquivalent("IS"_sl) ) op = "="_sl;
            else if ( op.caseEquivalent("IS NOT"_sl) )
                op = "!="_sl;
        }

        int n = 0;
        for ( auto& i = operands; i; ++i ) {
            // Write the operation/delimiter between arguments
            if ( n++ > 0 ) {
                if ( op != ","_sl )  // special case for argument lists
                    _sql << ' ';
                _sql << op << ' ';
            }
            parseCollatableNode(i.value());
        }

        if ( functionWantsCollation ) {
            if ( n > 0 ) { _sql << ", "; }

            _sql << "'" << _collation.sqliteName() << "'";
        }
    }

    static string columnTitleFromProperty(const Path& property) {
        if ( property.empty() )
            return "*";  // for the property ".", i.e. the entire doc. It will be translated to unique db alias.
        string first(property[0].keyStr());
        if ( first[0] == '_' ) {
            return first.substr(1);  // meta property
        } else if ( property[property.size() - 1].keyStr() ) {
            return string(property[property.size() - 1].keyStr());
        } else {
            return {};
        }
    }

    // Handles the WHAT clause (list of results)
    void QueryParser::resultOp(C4UNUSED slice op, Array::iterator& operands) {
        int      n         = 0;
        unsigned anonCount = 0;
        for ( auto& i = operands; i; ++i ) {
            // Write the operation/delimiter between arguments
            if ( n++ > 0 ) _sql << ", ";

            auto            result = i.value();
            string          title;
            Array::iterator expr(result->asArray());
            if ( expr && expr[0]->asString().caseEquivalent("AS"_sl) ) {
                // Handle 'AS':
                require(expr.count() == 3, "'AS' must have two operands");
                title = string(requiredString(expr[2], "'AS' alias"));

                result = expr[1];
                _sql << kResultFnName << "(";
                parseCollatableNode(result);
                _sql << ") AS " << sqlIdentifier(title);
                addAlias(title, kResultAlias, "");
            } else {
                _sql << (isImplicitBool(expr[0]) ? kBoolResultFnName : kResultFnName) << "(";
                if ( result->type() == kString ) {
                    // Convenience shortcut: interpret a string in a WHAT as a property path
                    writePropertyGetter(kValueFnName, Path(result->asString()));
                } else {
                    parseCollatableNode(result);
                }
                _sql << ")";

                // Come up with a column title if there is no 'AS':
                if ( result->type() == kString ) {
                    title = columnTitleFromProperty(Path(result->asString()));
                } else if ( result->type() == kArray ) {
                    if ( expr[0]->asString().hasPrefix('.') ) {
                        title = columnTitleFromProperty(propertyFromNode(result));
                    } else if ( expr[0]->asString().hasPrefix("_.") && expr.count() == 3 && expr[1]->type() == kArray
                                && expr[1]->asArray()->count() > 0
                                && expr[1]->asArray()->begin()->asString().compare("meta()") == 0 ) {
                        title = expr[2]->asString();
                        title = title.substr(1);
                    }
                }
                if ( title.empty() ) {
                    title = format("$%u", ++anonCount);  // default for non-properties
                } else if ( title == "*" ) {
                    title = _dbAlias;

                    for ( bool done = false; !done; done = true ) {
                        // special requirement: attempt to use sheer collection name if it's unambiguous.
                        const auto& iter = _aliases.find(_dbAlias);
                        require(iter != _aliases.end(), "alias must have been registered");

                        // First, if the alias is derived implicitly from the collection,
                        // as opposed to AS aliases
                        if ( _dbAlias != DataFile::unescapeCollectionName(iter->second.collection) ) break;

                        // Second, the collection is represented as collection path
                        auto pathSeparator = DataFile::findCollectionPathSeparator(iter->second.collection);
                        if ( pathSeparator == string::npos ) break;

                        // Finally, there is no joined datasource that has the same collection name.
                        string collectionName = iter->second.collection.substr(pathSeparator + 1);
                        auto   it             = _aliases.begin();
                        for ( ; it != _aliases.end(); ++it ) {
                            if ( it->second.type != kJoinAlias ) continue;
                            auto sep = DataFile::findCollectionPathSeparator(it->second.collection);
                            if ( collectionName
                                 == (sep == string::npos ? it->second.collection
                                                         : it->second.collection.substr(sep + 1)) ) {
                                break;
                            }
                        }
                        if ( it != _aliases.end() ) break;

                        // Assign the collection name to title
                        title = DataFile::unescapeCollectionName(collectionName);
                    }
                }  //if ( title == "*" )
            }

            // Make the title unique:
            string   uniqueTitle = title;
            unsigned dup         = 2;
            while ( find(_columnTitles.begin(), _columnTitles.end(), uniqueTitle) != _columnTitles.end() )
                uniqueTitle = title + format(" #%u", dup++);
            _columnTitles.push_back(uniqueTitle);
        }
    }

    // Handles array literals (the "[]" op)
    // But note that this op is treated specially if it's an operand of "IN" (see inOp)
    void QueryParser::arrayLiteralOp(C4UNUSED slice op, Array::iterator& operands) {
        functionOp(kArrayFnNameWithParens, operands);
    }

    // Handles EXISTS
    void QueryParser::existsOp(slice op, Array::iterator& operands) {
        // "EXISTS propertyname" turns into a call to fl_exists()
        if ( writeNestedPropertyOpIfAny(kExistsFnName, operands) ) return;

        _sql << "EXISTS";
        if ( isalpha(op[op.size - 1]) ) _sql << ' ';
        parseNode(operands[0]);
    }

    static void setFlagFromOption(bool& flag, const Dict* options, slice key) {
        const Value* val = getCaseInsensitive(options, key);
        if ( val ) flag = val->asBool();
    }

    // Handles COLLATE
    void QueryParser::collateOp(C4UNUSED slice op, Array::iterator& operands) {
        auto outerCollation     = _collation;
        auto outerCollationUsed = _collationUsed;

        // Apply the collation options, overriding the inherited ones:
        const Dict* options = requiredDict(operands[0], "COLLATE options");
        setFlagFromOption(_collation.unicodeAware, options, "UNICODE"_sl);
        setFlagFromOption(_collation.caseSensitive, options, "CASE"_sl);
        setFlagFromOption(_collation.diacriticSensitive, options, "DIAC"_sl);

        auto localeName = getCaseInsensitive(options, "LOCALE"_sl);
        if ( localeName ) _collation.localeName = localeName->asString();
        _collationUsed = false;

        // Remove myself from the operator stack so my precedence doesn't cause confusion:
        auto curContext = _context.back();
        _context.pop_back();

        // Parse the expression:
        parseNode(operands[1]);

        // If nothing in the expression (like a comparison operator) used the collation to generate
        // a SQL 'COLLATE', generate one now for the entire expression:
        if ( !_collationUsed ) writeCollation();

        _context.push_back(curContext);

        // Pop the collation options:
        _collation     = outerCollation;
        _collationUsed = outerCollationUsed;
    }

    // Handles "x || y", turning it into a call to the concat() function
    void QueryParser::concatOp(C4UNUSED slice op, Array::iterator& operands) { functionOp("concat()"_sl, operands); }

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
        bool notIn        = (op != "IN"_sl);
        auto arrayOperand = operands[1]->asArray();
        if ( arrayOperand && arrayOperand->count() > 0 && arrayOperand->get(0)->asString() == "[]"_sl ) {
            // RHS is a literal array, so use SQL "IN" syntax:
            parseCollatableNode(operands[0]);
            _sql << ' ' << op << ' ';
            Array::iterator arrayOperands(arrayOperand);
            writeArgList(++arrayOperands);

        } else {
            // Otherwise generate a call to array_contains():
            _context.push_back(&kArgListOperation);  // prevents extra parens around operands

            if ( notIn ) _sql << "(NOT ";

            _sql << "array_contains(";
            parseNode(operands[1]);  // yes, operands are in reverse order
            _sql << ", ";
            parseCollatableNode(operands[0]);
            _sql << ")";

            if ( notIn ) _sql << ")";

            _context.pop_back();
        }
    }

    void QueryParser::likeOp(C4UNUSED slice op, Array::iterator& operands) {
        // Optimization: use SQLite's built-in LIKE function when possible, i.e. when the collation
        // in effect matches SQLite's BINARY collation. This allows the query optimizer to use the
        // "LIKE optimization", allowing an indexed prefix search, when the pattern is a literal or
        // parameter and doesn't begin with a wildcard. (CBL-890)
        // <https://sqlite.org/optoverview.html#like_opt>
        if ( _collation.caseSensitive && _collation.diacriticSensitive && !_collation.unicodeAware ) {
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
    void QueryParser::matchOp(C4UNUSED slice op, Array::iterator& operands) {
        // Is a MATCH legal here? Look at the parent operation(s):
        auto parentCtx = _context.rbegin() + 1;
        auto parentOp  = (*parentCtx)->op;
        while ( parentOp == "AND"_sl ) parentOp = (*++parentCtx)->op;
        require(parentOp == "SELECT"_sl || parentOp == nullslice,
                "MATCH can only appear at top-level, or in a top-level AND");

        // Write the expression:
        auto ftsTableAlias = FTSJoinTableAlias(operands[0]);
        Assert(!ftsTableAlias.empty());
        _sql << ftsTableAlias << "." << sqlIdentifier(FTSTableName(operands[0]).first) << " MATCH ";
        parseCollatableNode(operands[1]);
    }

    // Handles "ANY var IN array SATISFIES expr" (and EVERY, and ANY AND EVERY)
    void QueryParser::anyEveryOp(slice op, Array::iterator& operands) {
        auto var = (string)requiredString(operands[0], "ANY/EVERY first parameter");
        require(isValidIdentifier(var), "ANY/EVERY first parameter must be an identifier; '%s' is not", var.c_str());
        require(_variables.count(var) == 0, "Variable '%s' is already in use", var.c_str());
        _variables.insert(var);

        const Value* arraySource = operands[1];
        auto         predicate   = requiredArray(operands[2], "ANY/EVERY third parameter");

        bool every       = !op.caseEquivalent("ANY"_sl);
        bool anyAndEvery = op.caseEquivalent("ANY AND EVERY"_sl);

        if ( op.caseEquivalent("ANY"_sl) && predicate->count() == 3 && predicate->get(0)->asString() == "="_sl
             && string(propertyFromNode(predicate->get(1), '?')) == var ) {
            // If predicate is `var = value`, generate `fl_contains(array, value)` instead
            writeFunctionGetter(kContainsFnName, arraySource, predicate->get(2));
            return;
        }

        if ( anyAndEvery ) {
            _sql << '(';
            writeFunctionGetter(kCountFnName, arraySource);
            _sql << " > 0 AND ";
        }

        if ( every ) _sql << "NOT ";
        _sql << "EXISTS (SELECT 1 FROM ";
        writeEachExpression(arraySource);
        _sql << " AS _" << var << " WHERE ";
        if ( every ) _sql << "NOT (";
        parseNode(predicate);
        if ( every ) _sql << ')';
        _sql << ')';
        if ( anyAndEvery ) _sql << ')';

        _variables.erase(var);
    }

    // Handles doc property accessors, e.g. [".", "prop"] or [".prop"] --> fl_value(body, "prop")
    void QueryParser::propertyOp(C4UNUSED slice op, Array::iterator& operands) {
        writePropertyGetter(kValueFnName, propertyFromOperands(operands));
    }

    bool QueryParser::optimizeMetaKeyExtraction(Array::iterator& operands) {
        // Handle Meta().id - N1QL
        // ["_.", ["meta()", <db>], ".id"] - JSON

        const Array* metaop = operands[0]->asArray();
        if ( metaop == nullptr || metaop->count() == 0
             || !metaop->begin().value()->asString().caseEquivalent("meta()") ) {
            return false;
        }
        slice dbAlias;
        if ( metaop->count() > 1 ) {
            const Value* second = metaop->get(1);
            if ( second->type() == kString ) { dbAlias = second->asString(); }
        }
        slice meta_key = operands[1]->asString();
        if ( meta_key == nullptr ) { return false; }
        if ( meta_key[0] == '.' ) { meta_key.moveStart(1); }
        string      dbAlias_s = dbAlias.asString();
        Path        path{slice(dbAlias_s + ".id")};
        const auto& dbIter = verifyDbAlias(path);
        require(dbAlias_s.empty() || dbAlias_s == dbIter->first,
                "database alias '%s' does not match a declared 'AS' alias", dbAlias_s.c_str());

        writeMetaPropertyGetter(meta_key, dbIter->first);
        return true;
    }

    // Handles object (dict) property accessors, e.g. ["_.", [...], "prop"] --> fl_nested_value(..., "prop")
    void QueryParser::objectPropertyOp(slice op, Array::iterator& operands) {
        auto nOperands = operands.count();

        if ( nOperands == 2 && optimizeMetaKeyExtraction(operands) ) { return; }

        _sql << kNestedValueFnName << '(';
        _context.push_back(&kArgListOperation);  // prevents extra parens around operands
        require(nOperands > 0, "Missing dictionary parameter for '%.*s'", SPLAT(op));
        parseNode(operands[0]);
        _context.pop_back();

        slice path;
        if ( op.size == 2 ) {
            require(nOperands == 2, "Missing object-property path parameter");
            path = requiredString(operands[1], "object property path");
        } else {
            require(nOperands == 1, "Excess object-property parameter");
            path = op;
            path.moveStart(2);
        }

        _sql << ", " << sqlString(path) << ")";
    }

    void QueryParser::blobOp(C4UNUSED slice op, Array::iterator& operands) {
        writePropertyGetter(kBlobFnName, Path(requiredString(operands[0], "blob path")));
    }

    // Handles substituted query parameters, e.g. ["$", "x"] or ["$x"] --> $_x
    void QueryParser::parameterOp(slice op, Array::iterator& operands) {
        alloc_slice parameter;
        if ( op.size == 1 ) {
            parameter = operands[0]->toString();
        } else {
            op.moveStart(1);
            parameter = op;
            require(operands.count() == 0, "extra operands to '%.*s'", SPLAT(parameter));
        }
        auto paramStr = (string)parameter;
        require(isAlphanumericOrUnderscore(parameter), "Invalid query parameter name '%.*s'", SPLAT(parameter));
        _parameters.insert(paramStr);
        _sql << "$_" << paramStr;
    }

    // Handles variables used in ANY/EVERY predicates
    void QueryParser::variableOp(slice op, Array::iterator& operands) {
        // Concatenate the op and operands as a path:
        Path path;
        if ( op.size > 1 ) {
            op.moveStart(1);
            path += Path(op.asString());
        }
        if ( operands.count() > 0 ) { path += propertyFromOperands(operands); }

        // Split the path into variable name and property:
        string var(path[0].keyStr());
        path.drop(1);

        require(isValidIdentifier(var), "Invalid variable name '%.*s'", SPLAT(op));
        require(_variables.count(var) > 0, "No such variable '%.*s'", SPLAT(op));

        // Now generate the function call:
        if ( path.empty() ) {
            _sql << '_' << var << ".value";
        } else {
            _sql << kNestedValueFnName << "(_" << var << ".body, " << sqlString(string(path)) << ")";
        }
    }

    // Handles MISSING, which is the N1QL equivalent of NULL
    void QueryParser::missingOp(C4UNUSED slice op, Array::iterator& operands) { _sql << "NULL"; }

    // Handles CASE
    void QueryParser::caseOp(C4UNUSED fleece::slice op, Array::iterator& operands) {
        // First operand is either the expression being tested, or null if there isn't one.
        // After that, operands come in pairs of 'when', 'then'.
        // If there's one remaining, it's the 'else'.
        _sql << "CASE";
        if ( operands[0]->type() != kNull ) {
            _sql << ' ';
            parseNode(operands[0]);
        }
        ++operands;
        bool hasElse = false;
        while ( operands ) {
            auto test = operands.value();
            ++operands;
            if ( operands ) {
                _sql << " WHEN ";
                parseNode(test);
                _sql << " THEN ";
                parseNode(operands.value());
                ++operands;
            } else {
                _sql << " ELSE ";
                parseNode(test);
                hasElse = true;
            }
        }
        // Fill up the vaccum due to the absense of ELSE, or SQLite will fill it with SQLite's NULL but we want
        // the JSON null.
        if ( !hasElse ) { _sql << " ELSE " << kNullFnName << "()"; }
        _sql << " END";
    }

    // Handles SELECT
    void QueryParser::selectOp(C4UNUSED fleece::slice op, Array::iterator& operands) {
        // SELECT is unusual in that its operands are encoded as an object
        auto dict = requiredDict(operands[0], "Argument to SELECT");
        if ( _context.size() <= 2 ) {
            // Outer SELECT
            writeSelect(dict);
        } else {
            // Nested SELECT; use a fresh parser
            QueryParser nested(this);
            nested.parse(dict);
            _sql << nested.SQL();
            _kvTables.insert(nested._kvTables.begin(), nested._kvTables.end());
        }
    }

    namespace {
        slice const kMetaKeys[] = {"id"_sl, "sequence"_sl, "deleted"_sl, "expiration"_sl, "revisionID"_sl};

        enum { mkId, mkSequence, mkDeleted, mkExpiration, mkRevisionId, mkCount };

        static_assert(sizeof(kMetaKeys) / sizeof(kMetaKeys[0]) == mkCount);
    }  // namespace

    // Handles ["meta", dbAlias_optional]
    void QueryParser::metaOp(C4UNUSED slice op, Array::iterator& operands) {
        // Pre-conditions: op == "meta"
        //                 operands.size() == 0 || operands[0]->type() == kString (dbAlias)

        string arg;
        if ( operands.count() > 0 && operands[0]->type() == kString ) { arg = operands[0]->asString(); }

        Path        path{slice(arg + ".id")};
        const auto& dbIter = verifyDbAlias(path);
        require(arg.empty() || arg == dbIter->first, "database alias '%s' does not match a declared 'AS' alias",
                arg.c_str());

        _sql << kDictFnName << '(';
        bool first = true;
        for ( slice k : kMetaKeys ) {
            if ( !first ) {
                _sql << ", ";
            } else {
                first = false;
            }
            _sql << sqlString(k) << ", ";
            writeMetaPropertyGetter(k, dbIter->first);
        }
        _sql << ')';
    }

    void QueryParser::writeMetaPropertyGetter(slice metaKey, const string& dbAlias) {
        string tablePrefix;
        if ( !dbAlias.empty() ) { tablePrefix = quotedIdentifierString(dbAlias) + "."; }

        auto b  = &kMetaKeys[0];
        auto it = find_if(b, b + mkCount, [metaKey](auto& p) { return p == metaKey; });
        require(it != b + mkCount, "'%s' is not a valid Meta key", metaKey.asString().c_str());

        switch ( int i = (int)(it - b) ) {
            case mkId:
                writeMetaProperty(kValueFnName, tablePrefix, "key");
                break;
            case mkDeleted:
                writeDeletionTest(dbAlias, true);
                _checkedDeleted = true;
                break;
            case mkRevisionId:
                _sql << kVersionFnName << "(" << tablePrefix << "version"
                     << ")";
                break;
            case mkSequence:
                writeMetaProperty(kValueFnName, tablePrefix, kMetaKeys[i]);
                break;
            case mkExpiration:
                writeMetaProperty(kValueFnName, tablePrefix, kMetaKeys[i]);
                _checkedExpiration = true;
                break;
            default:
                Assert(false, "Internal logic error");
        };
    }

    // Handles unrecognized operators, based on prefix ('.', '$', '?') or suffix ('()').
    void QueryParser::fallbackOp(slice op, Array::iterator& operands) {
        // Put the actual op into the context instead of a null
        auto operation  = *_context.back();
        operation.op    = op;
        _context.back() = &operation;

        if ( op.hasPrefix('.') ) {
            op.moveStart(1);  // Skip initial .
            if ( op.hasPrefix("$") ) {
                alloc_slice escaped = escapedPath(op);
                writePropertyGetter(kValueFnName, Path(escaped));
            } else {
                writePropertyGetter(kValueFnName, Path(op));
            }
        } else if ( op.hasPrefix("_."_sl) ) {
            objectPropertyOp(op, operands);
        } else if ( op.hasPrefix('$') ) {
            parameterOp(op, operands);
        } else if ( op.hasPrefix('?') ) {
            variableOp(op, operands);
        } else if ( op.hasSuffix("()"_sl) ) {
            functionOp(op, operands);
        } else {
            fail("Unknown operator '%.*s'", SPLAT(op));
        }
    }

    // Handles function calls, where the op ends with "()"
    void QueryParser::functionOp(slice op, Array::iterator& operands) {
        // Look up the function name:
        if ( op.hasSuffix("()"_sl) ) { op.shorten(op.size - 2); }
        string              fnName = op.asString();
        const FunctionSpec* spec;
        for ( spec = kFunctionList; spec->name; ++spec ) {
            if ( op.caseEquivalent(spec->name) ) break;
        }
        require(spec->name, "Unknown function '%.*s'", SPLAT(op));
        if ( spec->aggregate ) {
            require(_aggregatesOK, "Cannot use aggregate function %.*s() in this context", SPLAT(op));
            _isAggregateQuery = true;
        }
        auto arity = operands.count();
        require(arity >= spec->minArgs, "Too few arguments for function '%.*s'", SPLAT(op));
        require(arity <= spec->maxArgs || spec->maxArgs >= 9, "Too many arguments for function '%.*s'", SPLAT(op));

        if ( spec->name == "match"_sl ) {
            matchOp(op, operands);
            return;
        }

        if ( spec->sqlite_name ) op = spec->sqlite_name;
        else
            op = spec->name;  // canonical case

        // Special case: "array_count(propertyname)" turns into a call to fl_count:
        if ( op.caseEquivalent(kArrayCountFnName) && writeNestedPropertyOpIfAny(kCountFnName, operands) ) return;

        // Special case: in "rank(ftsName)" the param has to be a matchinfo() call:
        if ( op.caseEquivalent(kRankFnName) ) {
            string fts = FTSTableName(operands[0]).first;
            auto   i   = _indexJoinTables.find(fts);
            if ( i == _indexJoinTables.end() ) fail("rank() can only be called on FTS indexes");
            _sql << "rank(matchinfo(" << i->second << "." << sqlIdentifier(i->first) << "))";
            return;
        }

        // Special case: "prediction()" may be indexed:
#ifdef COUCHBASE_ENTERPRISE
        if ( op.caseEquivalent(kPredictionFnName) && writeIndexedPrediction((const Array*)_curNode) ) return;
#endif

        if ( !_collationUsed && spec->wants_collation ) {
            _collationUsed          = true;
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

    void QueryParser::writeDictLiteral(const fleece::impl::Dict* dict) {
        _context.push_back(&kArgListOperation);
        _sql << kDictFnName << '(';
        int n = 0;
        for ( Dict::iterator i(dict); i; ++i ) {
            if ( n++ > 0 ) _sql << ", ";
            _sql << sqlString(i.keyString()) << ", ";
            parseNode(i.value());
        }
        _sql << ')';
        _context.pop_back();
    }

#pragma mark - PROPERTIES:

    namespace qp {
        // Concatenates property operands to produce the property path string
        Path propertyFromOperands(Array::iterator& operands, bool firstIsEncoded) {
            Path path;
            int  n = 0;
            for ( auto& i = operands; i; ++i, ++n ) {
                auto arr = i.value()->asArray();
                if ( arr ) {
                    require(n > 0, "Property path can't start with an array index");
                    require(arr->count() == 1, "Property array index must have exactly one item");
                    require(arr->get(0)->isInteger(), "Property array index must be an integer");
                    path.addIndex((int)arr->get(0)->asInt());
                } else {
                    slice name = i.value()->asString();
                    require(name, "Invalid JSON value in property path");
                    if ( firstIsEncoded ) {
                        name.moveStart(1);  // skip '.', '?', '$'
                        if ( name.hasPrefix("$") ) {
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
        Path propertyFromNode(const Value* node, char prefix) {
            Array::iterator i(node->asArray());
            if ( i.count() >= 1 ) {
                auto op = i[0]->asString();
                if ( op.hasPrefix(prefix) ) {
                    bool justDot = (op.size == 1);
                    if ( justDot ) ++i;
                    return propertyFromOperands(i, !justDot);
                }
            }
            return {};  // not a valid property node
        }
    }  // namespace qp

    // If the first operand is a property operation, writes it using the given SQL function name
    // and returns true; else returns false.
    bool QueryParser::writeNestedPropertyOpIfAny(slice fnName, Array::iterator& operands) {
        if ( operands.count() == 0 ) return false;
        Path property = propertyFromNode(operands[0]);
        if ( property.empty() ) return false;
        writePropertyGetter(fnName, std::move(property));
        return true;
    }

    void QueryParser::writeFunctionGetter(slice fn, const Value* source, const Value* param) {
        Path property = propertyFromNode(source);
        if ( property.empty() ) {
            _sql << fn << "(";
            parseNode(source);
            if ( param ) {
                _sql << ", null, ";
                parseNode(param);
            }
            _sql << ")";
        } else {
            writePropertyGetter(fn, std::move(property), param);
        }
    }

    void QueryParser::writeMetaProperty(slice fn, const string& tablePrefix, slice property) {
        require(fn == kValueFnName, "can't use '_%.*s' in this context", SPLAT(property));
        _sql << tablePrefix << property;
    }

    // Return the iterator to _aliases based on the property.
    // Post-condition:
    //     return_iterator != _aliases.end() && return_iterator->second.type != kResultAlias
    QueryParser::AliasMap::const_iterator QueryParser::verifyDbAlias(fleece::impl::Path& property,
                                                                     string*             error) const {
        string alias;
        auto   iType     = _aliases.end();
        int    leadMatch = 0;
        do {
            if ( property.empty() ) break;

            // full-match has the highest priority
            // Check for result alias before 'alias' gets reassigned below
            alias     = string(property[0].keyStr());
            leadMatch = 1;
            iType     = _aliases.find(alias);
            if ( iType != _aliases.end() && iType->second.type == kResultAlias ) {
                // we only look for alias in the db context.
                iType = _aliases.end();
            }
            if ( iType != _aliases.end() ) break;

            // Assertion: iType == _aliase.end()
            int matchedCount = 0;
            for ( auto it = _aliases.begin(); it != _aliases.end(); ++it ) {
                if ( it->second.type == kResultAlias ) continue;
                if ( it->first.length() > alias.length() + 1
                     && it->first.substr(it->first.length() - alias.length()) == alias
                     && it->first.at(it->first.length() - alias.length() - 1) == '.' ) {
                    // scope.property[0]
                    matchedCount++;
                    iType = it;
                }
            }

            //Aasertion: matchedCount == 0 || iType != _aliases.end()
            if ( matchedCount > 1 ) {
                // Ambiguous
                iType = _aliases.end();
            } else if ( matchedCount == 1 ) {
                // get unique match of x.property[0].
                // want to make sure x is a scope.
                // c.f. parseFromEntry
                if ( iType->first == DataFile::unescapeCollectionName(iType->second.collection) ) {
                    alias = iType->first;
                    break;
                }
            }

            // Assertion: iType == _aliase.end()
            if ( property.size() == 1 || !property[1].isKey() ) break;

            // Look for property[0].property[1]
            alias     = alias + "." + string(property[1].keyStr());
            leadMatch = 2;
            iType     = _aliases.find(alias);
            if ( iType != _aliases.end() && iType->second.type == kResultAlias ) {
                // we only look for alias in the db context.
                iType = _aliases.end();
            }

        } while ( false );

        bool hasMultiDbAliases = false;
        if ( _aliases.size() > 1 ) {
            int cnt = 0;
            for ( const auto& _aliase : this->_aliases ) {
                if ( _aliase.second.type != kResultAlias ) {
                    if ( ++cnt == 2 ) {
                        hasMultiDbAliases = true;
                        break;
                    }
                }
            }
        }
        bool dropAliasIfSuccess = false;
        if ( _propertiesUseSourcePrefix && !property.empty() ) {
            // Interpret the first component of the property as a db alias:
            require(property[0].isKey(), "Property path can't start with array index");
            if ( hasMultiDbAliases || alias == _dbAlias ) {
                // With join (size > 1), properties must start with a keyspace alias to avoid ambiguity.
                // Otherwise, we assume property[0], or property[0].property[1], to be the alias if it coincides
                // with the unique one. Otherwise, we consider that the property path starts in the document and,
                // hence, do not drop.
                dropAliasIfSuccess = true;
            } else {
                alias = _dbAlias;
            }
        } else {
            alias = _dbAlias;
        }

        if ( iType == _aliases.end() ) {
            iType = _aliases.find(alias);
            if ( iType != _aliases.end() && iType->second.type == kResultAlias ) {
                // we only look for alias in the db context.
                iType = _aliases.end();
            }
        }

        bool postCondition = (iType != _aliases.end());
        if ( !postCondition ) {
            string message =
                    format("property '%s' does not begin with a declared 'AS' alias", string(property).c_str());
            if ( error == nullptr ) {
                fail("%s", message.c_str());
                // no-return
            } else {
                *error = message;
            }
        } else if ( dropAliasIfSuccess ) {
            property.drop(leadMatch);
        }
        return iType;
    }

    // Writes a call to a Fleece SQL function, including the closing ")".
    void QueryParser::writePropertyGetter(slice fn, Path&& property, const Value* param) {
        size_t propertySizeIn = property.size();
        // We send "property" to verifyDbAlias(). This function ensure that, after return,
        // property is a path to the property in the doc. If the original property starts
        // with database alias, such as db.name.firstname, the function will strip
        // the leading database alias, db, and hence, property as a path will have its size
        // reduced by 1.
        auto&&        iType                           = verifyDbAlias(property);
        bool          propertyStartsWithExplicitAlias = (property.size() + 1 == propertySizeIn);
        const string& alias                           = iType->first;
        aliasType     type                            = iType->second.type;
        string        tablePrefix                     = alias.empty() ? "" : quotedIdentifierString(alias) + ".";

        if ( type >= kUnnestVirtualTableAlias ) {
            // The alias is to an UNNEST. This needs to be written specially:
            writeUnnestPropertyGetter(fn, property, alias, type);
            return;
        }

        // CBL-3040. We should not apply the following rule of result alias if the
        // property starts with a database collection alias explicitly. In this case,
        // the following name is the proerty name in the collection.
        if ( !propertyStartsWithExplicitAlias ) {
            // Check out the case the property starts with the result alias.
            auto resultAliasIter = _aliases.end();
            if ( !property.empty() ) {
                resultAliasIter = _aliases.find(property[0].keyStr().asString());
                if ( resultAliasIter != _aliases.end() && resultAliasIter->second.type != kResultAlias ) {
                    resultAliasIter = _aliases.end();
                }
            }

            if ( resultAliasIter != _aliases.end() ) {
                const string& resultAlias = resultAliasIter->first;
                // If the property in question is identified as an alias, emit that instead of
                // a standard getter since otherwise it will probably be wrong (i.e. doc["alias"]
                // vs alias -> doc["path"]["to"]["value"])
                if ( property.size() == 1 ) {
                    // Simple case, the alias is being used as-is
                    _sql << sqlIdentifier(resultAlias);
                    return;
                }

                // More complicated case.  A subpath of an alias that points to
                // a collection type (e.g. alias = {"foo": "bar"}, and want to
                // ORDER BY alias.foo
                property.drop(1);
                _sql << kNestedValueFnName << "(" << sqlIdentifier(resultAlias) << ", " << sqlString(string(property))
                     << ")";
                return;
            }
        }

        if ( property.size() == 1 ) {
            // Check if this is a document metadata property:
            slice meta = property[0].keyStr();
            if ( meta == kDocIDProperty ) {
                writeMetaProperty(fn, tablePrefix, "key");
                return;
            } else if ( meta == kSequenceProperty ) {
                writeMetaProperty(fn, tablePrefix, "sequence");
                return;
            } else if ( meta == kExpirationProperty ) {
                writeMetaProperty(fn, tablePrefix, "expiration");
                _checkedExpiration = true;
                return;
            } else if ( meta == kDeletedProperty ) {
                require(fn == kValueFnName, "can't use 'deleted' in this context");
                writeDeletionTest(alias, true);
                _checkedDeleted = true;
                return;
            } else if ( meta == kRevIDProperty ) {
                _sql << kVersionFnName << "(" << tablePrefix << "version"
                     << ")";
                return;
            }
        }

        // It's more efficent to get the doc root with fl_root than with fl_value:
        if ( property.empty() && fn == kValueFnName ) fn = kRootFnName;

        // Write the function call:
        _sql << fn << "(" << tablePrefix << _bodyColumnName;
        if ( !property.empty() ) { _sql << ", " << sqlString(string(property)); }
        if ( param ) {
            _sql << ", ";
            parseNode(param);
        }
        _sql << ")";
    }

    void QueryParser::writeUnnestPropertyGetter(slice fn, Path& property, const string& alias, aliasType type) {
        require(fn == kValueFnName, "can't use an UNNEST alias in this context");
        string spec(property);
        require(slice(spec) != kDocIDProperty && slice(spec) != kSequenceProperty, "can't use '%s' on an UNNEST",
                spec.c_str());
        string tablePrefix;
        if ( _propertiesUseSourcePrefix ) tablePrefix = quotedIdentifierString(alias) + ".";

        if ( type == kUnnestVirtualTableAlias ) {
            if ( property.empty() ) {
                _sql << tablePrefix << "value";
            } else {
                _sql << kNestedValueFnName << "(" << tablePrefix << "body, " << sqlString(spec) << ")";
            }
        } else {
            _sql << kUnnestedValueFnName << "(" << tablePrefix << "body";
            if ( !property.empty() ) { _sql << ", " << sqlString(spec); }
            _sql << ")";
        }
    }

    // Writes an 'fl_each()' call representing a virtual table for the array at the given property
    void QueryParser::writeEachExpression(Path&& property) {
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
        writePropertyGetter(kEachFnName, std::move(property));  // write fl_each()
    }

    // Writes an 'fl_each()' call representing a virtual table for the array at the given property
    void QueryParser::writeEachExpression(const Value* propertyExpr) { writeFunctionGetter(kEachFnName, propertyExpr); }

    std::string QueryParser::expressionSQL(const fleece::impl::Value* expr) {
        parseJustExpression(expr);
        return SQL();
    }

    std::string QueryParser::whereClauseSQL(const fleece::impl::Value* arrayExpr, string_view dbAlias) {
        reset();
        if ( !dbAlias.empty() ) addAlias(string(dbAlias), kDBAlias, _defaultTableName);
        writeWhereClause(arrayExpr);
        string sql = SQL();
        if ( sql[0] == ' ' ) sql.erase(sql.begin(), sql.begin() + 1);
        return sql;
    }

    std::string QueryParser::eachExpressionSQL(const fleece::impl::Value* arrayExpr) {
        reset();
        addDefaultAlias();
        writeEachExpression(arrayExpr);
        return SQL();
    }

    std::string QueryParser::FTSExpressionSQL(const fleece::impl::Value* ftsExpr) {
        reset();
        addDefaultAlias();
        writeFunctionGetter(kFTSValueFnName, ftsExpr);
        return SQL();
    }

    // Given an index table name, returns its join alias. If `aliasPrefix` is given, it will add
    // a new alias if necessary, which will begin with that prefix.
    const string& QueryParser::indexJoinTableAlias(const string& tableName, const char* aliasPrefix) {
        auto i = _indexJoinTables.find(tableName);
        if ( i == _indexJoinTables.end() ) {
            if ( !aliasPrefix ) {
                static string kEmptyString;
                return kEmptyString;
            }
            string alias = aliasPrefix + to_string(_indexJoinTables.size() + 1);
            i            = _indexJoinTables.insert({tableName, alias}).first;
        }
        return i->second;
    }

#pragma mark - FULL-TEXT-SEARCH:

    // Recursively looks for MATCH expressions and adds the properties being matched to
    // _indexJoinTables. Returns the number of expressions found.
    unsigned QueryParser::findFTSProperties(const Value* root) {
        return findNodes(root, "MATCH()"_sl, 1, [this](const Array* match) {
            FTSJoinTableAlias(match->get(1), true);  // add LHS
        });
    }

    // Returns the pair of the FTS table name and database alias given the LHS of a MATCH expression.
    pair<string, string> QueryParser::FTSTableName(const Value* key) const {
        Path keyPath(requiredString(key, "left-hand side of MATCH expression"));
        // Path to FTS table has at most two components: [collectionAlias .] IndexName
        size_t compCount = keyPath.size();
        require((0 < compCount && compCount <= 2), "Reference to FTS table may take at most one dotted prefix.");
        Path keyPathBeforeVerifyDbAlias = keyPath;
        auto iAlias                     = _aliases.end();

        string outError;
        iAlias = verifyDbAlias(keyPath, &outError);
        slice prefix;
        if ( iAlias != _aliases.end() ) {
            ptrdiff_t diff = keyPathBeforeVerifyDbAlias.size() - keyPath.size();
            Assert(diff < 2);
            if ( diff > 0 ) prefix = keyPathBeforeVerifyDbAlias[0].keyStr();
        } else {
            bool   uniq = true;
            string uniqAlias;
            for ( auto iter = _aliases.begin(); iter != _aliases.end(); ++iter ) {
                if ( iter->second.type != kResultAlias ) {
                    if ( iter->second.type == kDBAlias ) { iAlias = iter; }
                    if ( uniqAlias.empty() ) {
                        uniqAlias = iter->second.tableName;
                    } else if ( uniqAlias != iter->second.tableName ) {
                        uniq = false;
                        break;
                    }
                }
            }
            if ( !uniq ) {
                Assert(!outError.empty());
                fail("%s", outError.c_str());
            }
        }
        Assert(iAlias != _aliases.end());

        string indexName = string(keyPath);
        require(!indexName.empty() && indexName.find('"') == string::npos,
                "FTS index name may not contain double-quotes nor be empty");
        return {_delegate.FTSTableName(iAlias->second.tableName, indexName), string(prefix)};
    }

    // Returns or creates the FTS join alias given the LHS of a MATCH expression.
    const string& QueryParser::FTSJoinTableAlias(const Value* matchLHS, bool canAdd) {
        auto [tableName, prefix] = FTSTableName(matchLHS);
        const string& alias      = indexJoinTableAlias(tableName);
        if ( !canAdd || !alias.empty() ) return alias;
        _ftsTables.push_back(tableName);
        _ftsTableAliases.insert({tableName, prefix});
        return indexJoinTableAlias(tableName, "fts");
    }

    // Returns the column name of an FTS table to use for a MATCH expression.
    string QueryParser::FTSColumnName(const Value* expression) {
        slice op = requiredArray(expression, "FTS index expression")->get(0)->asString();
        require(op.hasPrefix('.'), "FTS index expression must be a property");
        string property(propertyFromNode(expression));
        require(!property.empty(), "invalid property expression");
        return property;
    }

#pragma mark - UNNEST QUERY:

    // Constructs a unique identifier of an expression, from a digest of its JSON.
    string QueryParser::expressionIdentifier(const Array* expression, unsigned maxItems) const {
        require(expression, "Invalid expression to index");
        SHA1Builder sha;
        unsigned    item = 0;
        for ( Array::iterator i(expression); i; ++i ) {
            if ( maxItems > 0 && ++item > maxItems ) break;
            alloc_slice json = i.value()->toJSON(true);
            if ( _propertiesUseSourcePrefix ) {
                // Strip ".doc" from property paths if necessary:
                string s = json.asString();
                replace(s, "[\"." + _dbAlias + ".", "[\".");
                sha << slice(s);
            } else {
                sha << json;
            }
        }
        return sha.finish().asBase64();
    }

    // Returns the index table name for an unnested array property.
    string QueryParser::unnestedTableName(const Value* arrayExpr) const {
        string table = _defaultTableName;
        Path   path  = propertyFromNode(arrayExpr);
        string propertyStr;
        if ( !path.empty() ) {
            // It's a property path
            if ( _propertiesUseSourcePrefix ) {
                if ( const auto& first = path[0]; first.isKey() ) {
                    if ( string keyStr(first.keyStr()); keyStr != _dbAlias ) {
                        if ( auto i = _aliases.find(keyStr); i != _aliases.end() ) { table = i->second.tableName; }
                    }
                }
                path.drop(1);
            }
            propertyStr = string(path);
            require(propertyStr.find('"') == string::npos, "invalid property path for array index");
        } else {
            // It's some other expression; make a unique digest of it:
            propertyStr = expressionIdentifier(arrayExpr->asArray());
        }
        return _delegate.unnestedTableName(table, propertyStr);
    }

#pragma mark - PREDICTIVE QUERY:


#ifndef COUCHBASE_ENTERPRISE
    void QueryParser::findPredictionCalls(const Value* root) {}
#endif

}  // namespace litecore
