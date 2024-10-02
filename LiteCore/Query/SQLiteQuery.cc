//
// SQLiteQuery.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Defer.hh"
#include "Logging.hh"
#include "Query.hh"
#include "QueryTranslator.hh"
#include "n1ql_parser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Doc.hh"
#include "Encoder.hh"
#include "JSONConverter.hh"
#include "MutableDict.hh"
#include "Stopwatch.hh"
#include "SQLiteCpp/Statement.h"
#include "SQLiteCpp/Column.h"
#include "fleece/FLMutable.h"
#include <sqlite3.h>
#include <memory>
#include <numeric>  // std::accumulate
#include <sstream>
#include <iostream>


extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"  // for unicodesn_tokenizerRunningQuery()
}

using namespace std;
using namespace fleece::impl;

namespace litecore {

    class SQLiteQueryEnumerator;

    // Implicit columns in full-text query result:
    enum { kFTSRowidCol, kFTSOffsetsCol };

    namespace {
        bool hasKeyCaseEquivalent(const Dict* dict, slice key) {
            for ( Dict::iterator i(dict); i; ++i ) {
                if ( i.key()->asString().caseEquivalent(key) ) { return true; }
            }
            return false;
        }
    }  // namespace

    class SQLiteQuery final : public Query {
      public:
        SQLiteQuery(SQLiteDataFile& dataFile, slice queryStr, QueryLanguage language, SQLiteKeyStore* defaultKeyStore)
            : Query(dataFile, queryStr, language) {
            static constexpr const char* kLanguageName[] = {"JSON", "N1QL"};
            logInfo("Compiling %s query: %.*s", kLanguageName[(int)language], SPLAT(queryStr));

            switch ( language ) {
                case QueryLanguage::kJSON:
                    _json = queryStr;
                    break;
                case QueryLanguage::kN1QL:
                    {
                        int           errPos;
                        FLMutableDict result = n1ql::parse(string(queryStr), &errPos);
                        DEFER { FLMutableDict_Release(result); };

                        if ( !result ) {
                            throw Query::parseError("N1QL syntax error", errPos);
                        } else if ( !hasKeyCaseEquivalent((MutableDict*)result, "from") ) {
                            throw error(error::LiteCore, error::InvalidQuery,
                                        stringprintf("%s", "N1QL error: missing the FROM clause"));
                        }
                        _json = ((MutableDict*)result)->toJSON(true);
                        logVerbose("N1QL query translated to: %.*s", SPLAT(_json));
                        break;
                    }
            }

            QueryTranslator qp(dataFile, defaultKeyStore->collectionName(), defaultKeyStore->tableName());
            qp.parseJSON(_json);
            string sql = qp.SQL();
            logInfo("Compiled as %s", sql.c_str());

            // Collect the KeyStores read by this query:
            for ( const string& table : qp.collectionTablesUsed() )
                _keyStores.push_back(&dataFile.keyStoreFromTable(table));

            // Collect the (required) query parameters:
            _parameters = qp.parameters();
            for ( auto p = _parameters.begin(); p != _parameters.end(); ) {
                if ( hasPrefix(*p, "opt_") ) p = _parameters.erase(p);  // Optional param, don't warn if it's unbound
                else
                    ++p;
            }

            // Collect the FTS tables used:
            _ftsTables = qp.ftsTablesUsed();
            for ( auto& ftsTable : _ftsTables ) {
                if ( !dataFile.tableExists(ftsTable) )
                    error::_throw(error::NoSuchIndex, "'match' test requires a full-text index");
            }

            // If expiration is queried, ensure the table(s) have the expiration column:
            if ( qp.usesExpiration() ) {
                for ( auto ks : _keyStores ) ks->addExpiration();
            }

            LogTo(SQL, "Compiled {Query#%u}: %s", getObjectRef(), sql.c_str());
            _statement = dataFile.compile(sql.c_str());

            _1stCustomResultColumn = qp.firstCustomResultColumn();
            _columnTitles          = qp.columnTitles();
        }

        void close() override {
            logInfo("Closing query (db is closing)");
            _statement.reset();
            _matchedTextStatement.reset();
            Query::close();
        }

        sequence_t lastSequence() const {
            // This number is just used for before/after comparisons, so
            // return the total last-sequence of all used KeyStores
            return std::accumulate(
                    _keyStores.begin(), _keyStores.end(), 0_seq,
                    [](sequence_t total, const KeyStore* ks) { return total + uint64_t(ks->lastSequence()); });
        }

        uint64_t purgeCount() const {
            // This number is just used for before/after comparisons, so
            // return the total purge-count of all used KeyStores
            return std::accumulate(_keyStores.begin(), _keyStores.end(), 0,
                                   [](uint64_t total, const KeyStore* ks) { return total + ks->purgeCount(); });
        }

        alloc_slice getMatchedText(const FullTextTerm& term) override {
            // Get the expression that generated the text
            if ( _ftsTables.empty() ) error::_throw(error::NoSuchIndex);
            string expr = _ftsTables[0];  // TODO: Support for multiple matches in a query

            if ( !_matchedTextStatement ) {
                auto&  df             = (SQLiteDataFile&)dataFile();
                string sql            = "SELECT * FROM \"" + expr + "\" WHERE docid=?";
                _matchedTextStatement = std::make_unique<SQLite::Statement>(df, sql, true);
            }

            alloc_slice matchedText;
            _matchedTextStatement->bind(1, (long long)term.dataSource);  // dataSource is docid
            if ( _matchedTextStatement->executeStep() )
                matchedText = alloc_slice(getColumnAsSlice(*_matchedTextStatement, term.keyIndex));
            else
                Warn("FTS index %s has no row for docid %" PRIu64, expr.c_str(), term.dataSource);
            _matchedTextStatement->reset();
            return matchedText;
        }

        unsigned columnCount() const noexcept override {
            return statement()->getColumnCount() - _1stCustomResultColumn;
        }

        const vector<string>& columnTitles() const noexcept override { return _columnTitles; }

        string explain() override {
            stringstream result;
            // https://www.sqlite.org/eqp.html
            string query = statement()->getQuery();
            result << query << "\n\n";

            string            sql = "EXPLAIN QUERY PLAN " + query;
            auto&             df  = (SQLiteDataFile&)dataFile();
            SQLite::Statement x(df, sql);
            while ( x.executeStep() ) {
                for ( int i = 0; i < 3; ++i ) result << x.getColumn(i).getInt() << "|";
                result << " " << x.getColumn(3).getText() << "\n";
            }

            result << '\n' << _json << '\n';
            return result.str();
        }

        QueryEnumerator* createEnumerator(const Options* options) override;

        shared_ptr<SQLite::Statement> statement() const {
            if ( !_statement ) error::_throw(error::NotOpen);
            return _statement;
        }

        unsigned objectRef() const { return getObjectRef(); }  // (for logging)

        set<string>    _parameters;             // Names of the bindable parameters
        vector<string> _ftsTables;              // Names of the FTS tables used
        unsigned       _1stCustomResultColumn;  // Column index of the 1st column declared in JSON

      protected:
        ~SQLiteQuery() override { disposing(); }

        string loggingClassName() const override { return "Query"; }

      private:
        alloc_slice                   _json;                  // Original JSON form of the query
        shared_ptr<SQLite::Statement> _statement;             // Compiled SQLite statement
        unique_ptr<SQLite::Statement> _matchedTextStatement;  // Gets the matched text
        vector<string>                _columnTitles;          // Titles of columns
        vector<KeyStore*>             _keyStores;
    };

#pragma mark - QUERY ENUMERATOR:

    // Query enumerator that reads from prerecorded Fleece data (generated by fastForward(), below)
    // Each array item is a row, which is itself an array of column values.
    class SQLiteQueryEnumerator final
        : public QueryEnumerator
        , Logging {
      public:
        SQLiteQueryEnumerator(SQLiteQuery* query, const Query::Options* options, sequence_t lastSequence,
                              uint64_t purgeCount, Doc* recording, unsigned long long rowCount, double elapsedTime)
            : QueryEnumerator(options, lastSequence, purgeCount)
            , Logging(QueryLog)
            , _recording(recording)
            , _iter(_recording->asArray())
            , _1stCustomResultColumn(query->_1stCustomResultColumn)
            , _hasFullText(!query->_ftsTables.empty()) {
            logInfo("Created on {Query#%u} with %llu rows (%zu bytes) in %.3fms", query->objectRef(), rowCount,
                    recording->data().size, elapsedTime * 1000);
        }

        ~SQLiteQueryEnumerator() override { logInfo("Deleted"); }

        int64_t getRowCount() const override {
            return _recording->asArray()->count() / 2;  // (every other row is a column bitmap)
        }

        void seek(int64_t rowIndex) override {
            auto rows = _recording->asArray();
            rowIndex *= 2;
            if ( rowIndex < 0 ) {
                rowIndex = 0;
                _first   = true;
            } else {
                if ( rowIndex >= rows->count() ) error::_throw(error::InvalidParameter);
                _first = false;
            }
            _iter = Array::iterator(rows);
            _iter += (uint32_t)rowIndex;
        }

        bool next() override {
            if ( _first ) _first = false;
            else
                _iter += 2;
            if ( !_iter ) {
                logVerbose("END");
                return false;
            }
            if ( willLog(LogLevel::Verbose) ) {
                alloc_slice json = _iter->asArray()->toJSON();
                logVerbose("--> %.*s", SPLAT(json));
            }
            return true;
        }

        Array::iterator columns() const noexcept override {
            Array::iterator i(_iter[0u]->asArray());
            i += _1stCustomResultColumn;
            return i;
        }

        uint64_t missingColumns() const noexcept override { return _iter[1u]->asUnsigned(); }

        bool obsoletedBy(const QueryEnumerator* otherE) override {
            if ( !otherE ) return false;
            auto other = dynamic_cast<const SQLiteQueryEnumerator*>(otherE);
            if ( other->purgeCount() != _purgeCount ) {
                // If other is null for some weird reason all bets are off.  Otherwise
                // a purge will make changes that are unrecognizable to either lastSequence
                // or the data doc, so don't consult them
                return true;
            }

            if ( other->lastSequence() <= _lastSequence ) {
                return false;
            } else if ( _recording->data() == other->_recording->data() ) {
                _lastSequence = (sequence_t)other->_lastSequence;
                _purgeCount   = (uint64_t)other->_purgeCount;
                return false;
            } else {
                return true;
            }
        }

        QueryEnumerator* refresh(Query* query) override {
            auto                              newOptions  = _options.after(_lastSequence).withPurgeCount(_purgeCount);
            auto                              sqliteQuery = (SQLiteQuery*)query;
            unique_ptr<SQLiteQueryEnumerator> newEnum(
                    (SQLiteQueryEnumerator*)sqliteQuery->createEnumerator(&newOptions));
            if ( obsoletedBy(newEnum.get()) ) {
                // Results have changed, so return new enumerator:
                return newEnum.release();
            }
            return nullptr;
        }

        QueryEnumerator* clone() override {
            auto* clon =
                    new SQLiteQueryEnumerator(&_options, _lastSequence.load(), _purgeCount.load(), _recording.get());
            clon->_1stCustomResultColumn = this->_1stCustomResultColumn;
            clon->_hasFullText           = this->_hasFullText;
            return clon;
        }

        bool hasFullText() const override { return _hasFullText; }

        const FullTextTerms& fullTextTerms() override {
            _fullTextTerms.clear();
            uint64_t dataSource = _iter->asArray()->get(kFTSRowidCol)->asInt();
            if ( kFTSRowidCol < _1stCustomResultColumn ) {
                // The offsets() function returns a string of space-separated numbers in groups of 4.
                string      offsets = _iter->asArray()->get(kFTSOffsetsCol)->asString().asString();
                const char* termStr = offsets.c_str();
                while ( *termStr ) {
                    uint32_t n[4];
                    for ( unsigned int& i : n ) {
                        char* next;
                        i       = (uint32_t)strtol(termStr, &next, 10);
                        termStr = next;
                    }
                    _fullTextTerms.push_back({dataSource, n[0], n[1], n[2], n[3]});
                    // {rowid, key #, term #, byte offset, byte length}
                }
            }
            return _fullTextTerms;
        }

      protected:
        string loggingClassName() const override { return "QueryEnum"; }

      private:
        SQLiteQueryEnumerator(const Query::Options* options, sequence_t lastSequence, uint64_t purgeCount,
                              Doc* recording)
            : QueryEnumerator(options, lastSequence, purgeCount)
            , Logging(QueryLog)
            , _recording(recording)
            , _iter(_recording->asArray()) {}

        Retained<Doc>   _recording;
        Array::iterator _iter;
        unsigned        _1stCustomResultColumn{0};  // Column index of the 1st column declared in JSON
        bool            _hasFullText{false};
        bool            _first{true};
    };

    // Reads from 'live' SQLite statement and records the results into a Fleece array,
    // which is then used as the data source of a SQLiteQueryEnum.
    class SQLiteQueryRunner {
      public:
        SQLiteQueryRunner(SQLiteQuery* query, const Query::Options* options, sequence_t lastSequence,
                          uint64_t purgeCount)
            : _query(query)
            , _options(options ? *options : Query::Options())
            , _lastSequence(lastSequence)
            , _purgeCount(purgeCount)
            , _statement(query->statement())
            , _sk(query->dataFile().documentKeys()) {
            _statement->clearBindings();
            _unboundParameters = query->_parameters;
            if ( options && options->paramBindings.buf ) bindParameters(options->paramBindings);
            if ( !_unboundParameters.empty() ) {
                stringstream msg;
                for ( const string& param : _unboundParameters ) msg << " $" << param;
                Warn("Some query parameters were left unbound and will have value `MISSING`:%s", msg.str().c_str());
            }

            LogStatement(*_statement);
        }

        ~SQLiteQueryRunner() {
            try {
                _statement->reset();
            } catch ( ... ) {}
        }

        void bindParameters(slice json) {
            alloc_slice fleeceData;
            if ( json[0] == '{' && json[json.size - 1] == '}' ) fleeceData = JSONConverter::convertJSON(json);
            else
                fleeceData = json;
            const Dict* root = Value::fromData(fleeceData)->asDict();
            if ( !root ) error::_throw(error::InvalidParameter);
            for ( Dict::iterator it(root); it; ++it ) {
                auto key = (string)it.keyString();
                _unboundParameters.erase(key);
                auto         sqlKey = string("$_") + key;
                const Value* val    = it.value();
                try {
                    switch ( val->type() ) {
                        case kNull:
                            break;
                        case kBoolean:
                        case kNumber:
                            if ( val->isInteger() && !val->isUnsigned() )
                                _statement->bind(sqlKey, (long long)val->asInt());
                            else
                                _statement->bind(sqlKey, val->asDouble());
                            break;
                        case kString:
                            _statement->bind(sqlKey, (string)val->asString());
                            break;
                        default:
                            {
                                // Encode other types as a Fleece blob:
                                Encoder enc;
                                enc.writeValue(val);
                                alloc_slice asFleece = enc.finish();
                                _statement->bind(sqlKey, asFleece.buf, (int)asFleece.size);
                                break;
                            }
                    }
                } catch ( const SQLite::Exception& x ) {
                    if ( x.getErrorCode() == SQLITE_RANGE )
                        error::_throw(error::InvalidQueryParam, "Unknown query property '%s'", key.c_str());
                    else
                        throw;
                }
            }
        }

        bool encodeColumn(Encoder& enc, int i) {
            SQLite::Column col = _statement->getColumn(i);
            switch ( col.getType() ) {
                case SQLITE_NULL:
                    enc.writeNull();
                    return false;  // this column value is missing
                case SQLITE_INTEGER:
                    enc.writeInt(col.getInt64());
                    break;
                case SQLITE_FLOAT:
                    enc.writeDouble(col.getDouble());
                    break;
                case SQLITE_BLOB:
                    if ( i >= _query->_1stCustomResultColumn ) {
                        slice fleeceData{col.getBlob(), (size_t)col.getBytes()};
                        if ( fleeceData.empty() ) {
                            enc.writeNull();
                        } else {
                            Scope        fleeceScope(fleeceData, _sk);
                            const Value* value = Value::fromTrustedData(fleeceData);
                            if ( !value )
                                error::_throw(error::CorruptRevisionData,
                                              "SQLiteQueryRunner encodeColumn parsing fleece to Value failing");
                            enc.writeValue(value);
                        }
                        break;
                    }
                    // else fall through:
                case SQLITE_TEXT:
                    enc.writeString(slice{col.getText(), (size_t)col.getBytes()});
                    break;
            }
            return true;
        }

        // Collects all the (remaining) rows into a Fleece array of arrays,
        // and returns an enumerator impl that will replay them.
        SQLiteQueryEnumerator* fastForward() {
            fleece::Stopwatch st;
            int               nCols    = _statement->getColumnCount();
            uint64_t          rowCount = 0;
            // Give this encoder its own SharedKeys instead of using the database's DocumentKeys,
            // because the query results might include dicts with new keys that aren't in the
            // DocumentKeys.
            Encoder enc;
            auto    sk = retained(new SharedKeys);
            enc.setSharedKeys(sk);
            enc.beginArray();

            unicodesn_tokenizerRunningQuery(true);
            try {
                auto firstCustomCol = _query->_1stCustomResultColumn;
                while ( _statement->executeStep() ) {
                    uint64_t missingCols = 0;
                    enc.beginArray(nCols);
                    for ( int i = 0; i < nCols; ++i ) {
                        int64_t offsetColumn = i - firstCustomCol;
                        if ( !encodeColumn(enc, i) && offsetColumn >= 0 && offsetColumn < 64 ) {
                            missingCols |= (1ULL << offsetColumn);
                        }
                    }
                    enc.endArray();
                    // Add an integer containing a bit-map of which columns are missing/undefined:
                    enc.writeUInt(missingCols);
                    ++rowCount;
                }
            } catch ( ... ) {
                unicodesn_tokenizerRunningQuery(false);
                throw;
            }
            unicodesn_tokenizerRunningQuery(false);

            enc.endArray();
            return new SQLiteQueryEnumerator(_query, &_options, _lastSequence, _purgeCount, enc.finishDoc().get(),
                                             rowCount, st.elapsed());
        }

      private:
        Retained<SQLiteQuery>         _query;
        Query::Options                _options;
        sequence_t                    _lastSequence;  // DB's lastSequence at the time the query ran
        uint64_t                      _purgeCount;    // DB's purgeCount at the time the query ran
        shared_ptr<SQLite::Statement> _statement;
        set<string>                   _unboundParameters;
        SharedKeys*                   _sk;
    };

    // The factory method that creates a SQLite Query.
    Retained<Query> SQLiteDataFile::compileQuery(slice selectorExpression, QueryLanguage language, KeyStore* keyStore) {
        if ( !keyStore ) keyStore = &defaultKeyStore();
        return new SQLiteQuery(*this, selectorExpression, language, asSQLiteKeyStore(keyStore));
    }

    // The factory method that creates a SQLite QueryEnumerator, but only if the database has
    // changed since lastSeq.
    QueryEnumerator* SQLiteQuery::createEnumerator(const Options* options) {
        // Start a read-only transaction, to ensure that the result of lastSequence() and purgeCount() will be
        // consistent with the query results.
        ReadOnlyTransaction t(dataFile());

        sequence_t curSeq   = lastSequence();
        uint64_t   purgeCnt = purgeCount();
        if ( options && options->notOlderThan(curSeq, purgeCnt) ) return nullptr;
        SQLiteQueryRunner recorder(this, options, curSeq, purgeCnt);
        return recorder.fastForward();
    }

}  // namespace litecore
