//
// SQLiteQuery.cc
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

// THIS FILE HAS NOT MERGED THE CHANGES FROM feature/one-shot-query!
// THE SOURCE FILE FROM THE BRANCH IS AT SQLiteQuery_ONESHOT.cc

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Logging.hh"
#include "Query.hh"
#include "QueryParser.hh"
#include "n1ql_parser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "FleeceImpl.hh"
#include "MutableDict.hh"
#include "Path.hh"
#include "Stopwatch.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sqlite3.h>
#include <sstream>
#include <iostream>

extern "C" {
#include "sqlite3_unicodesn_tokenizer.h"        // for unicodesn_tokenizerRunningQuery()
}

using namespace std;
using namespace fleece::impl;

namespace litecore {

    class SQLiteQueryEnumerator;


    // Implicit columns in full-text query result:
    enum {
        kFTSRowidCol,
        kFTSOffsetsCol
    };


    class SQLiteQuery : public Query {
    public:
        SQLiteQuery(SQLiteKeyStore &keyStore, slice queryStr, QueryLanguage language)
        :Query(keyStore, queryStr, language)
        {
            static constexpr const char* kLanguageName[] = {"JSON", "N1QL"};
            logInfo("Compiling %s query: %.*s", kLanguageName[(int)language], SPLAT(queryStr));

            switch (language) {
                case QueryLanguage::kJSON:
                    _json = queryStr;
                    break;
                case QueryLanguage::kN1QL: {
                    unsigned errPos;
                    FLMutableDict result = n1ql::parse(string(queryStr), &errPos);
                    if (!result)
                        throw Query::parseError("N1QL syntax error", errPos);
                    _json = ((MutableDict*)result)->toJSON(true);
                    FLMutableDict_Release(result);
                    break;
                }
            }

            QueryParser qp(keyStore);
            qp.parseJSON(_json);

            _parameters = qp.parameters();
            for (auto p = _parameters.begin(); p != _parameters.end();) {
                if (hasPrefix(*p, "opt_"))
                    p = _parameters.erase(p);       // Optional param, don't warn if it's unbound
                else
                    ++p;
            }

            _ftsTables = qp.ftsTablesUsed();
            for (auto &ftsTable : _ftsTables) {
                if (!keyStore.db().tableExists(ftsTable))
                    error::_throw(error::NoSuchIndex, "'match' test requires a full-text index");
            }

            if (qp.usesExpiration())
                keyStore.addExpiration();

            string sql = qp.SQL();
            logInfo("Compiled as %s", sql.c_str());
            LogTo(SQL, "Compiled {Query#%u}: %s", getObjectRef(), sql.c_str());
            _statement.reset(keyStore.compile(sql));
            
            _1stCustomResultColumn = qp.firstCustomResultColumn();
            _columnTitles = qp.columnTitles();
        }


        virtual void close() override {
            logInfo("Closing query (db is closing)");
            _statement.reset();
            _matchedTextStatement.reset();
            Query::close();
        }


        sequence_t lastSequence() const {
            return keyStore().lastSequence();
        }


        uint64_t purgeCount() const {
            return keyStore().purgeCount();
        }


        alloc_slice getMatchedText(const FullTextTerm &term) override {
            // Get the expression that generated the text
            if (_ftsTables.size() == 0)
                error::_throw(error::NoSuchIndex);
            string expr = _ftsTables[0];    // TODO: Support for multiple matches in a query

            if (!_matchedTextStatement) {
                auto &df = (SQLiteDataFile&) keyStore().dataFile();
                string sql = "SELECT * FROM \"" + expr + "\" WHERE docid=?";
                _matchedTextStatement.reset(new SQLite::Statement(df, sql, true));
            }

            alloc_slice matchedText;
            _matchedTextStatement->bind(1, (long long)term.dataSource); // dataSource is docid
            if (_matchedTextStatement->executeStep())
                matchedText = alloc_slice( dynamic_cast<SQLiteKeyStore&>(keyStore()).columnAsSlice(_matchedTextStatement->getColumn(term.keyIndex)) );
            else
                Warn("FTS index %s has no row for docid %" PRIu64, expr.c_str(), term.dataSource);
            _matchedTextStatement->reset();
            return matchedText;
        }


        virtual unsigned columnCount() const noexcept override {
            return statement()->getColumnCount() - _1stCustomResultColumn;
        }


        virtual const vector<string>& columnTitles() const noexcept override {
            return _columnTitles;
        }


        string explain() override {
            stringstream result;
            // https://www.sqlite.org/eqp.html
            string query = statement()->getQuery();
            result << query << "\n\n";

            string sql = "EXPLAIN QUERY PLAN " + query;
            auto &df = (SQLiteDataFile&) keyStore().dataFile();
            SQLite::Statement x(df, sql);
            while (x.executeStep()) {
                for (int i = 0; i < 3; ++i)
                    result << x.getColumn(i).getInt() << "|";
                result << " " << x.getColumn(3).getText() << "\n";
            }

            result << '\n' << _json << '\n';
            return result.str();
        }

        QueryEnumerator* createEnumerator(const Options *options) override;

        shared_ptr<SQLite::Statement> statement() const {
            if (!_statement)
                error::_throw(error::NotOpen);
            return _statement;
        }

        void bindParameters(const Options *options);

        // Records the query rows from my statement into a Fleece Doc.
        Retained<Doc> fastForward() {
            fleece::Stopwatch st;
            int nCols = _statement->getColumnCount();
            auto sk = keyStore().dataFile().documentKeys();
            Encoder enc;
            enc.beginArray();

            unicodesn_tokenizerRunningQuery(true);
            try {
                while (_statement->executeStep()) {
                    enc.beginArray(nCols);
                    for (int i = 0; i < nCols; ++i) {
                        // Encode a column value:
                        auto col = _statement->getColumn(i);
                        switch (col.getType()) {
                            case SQLITE_NULL:
                                enc.writeUndefined();
                                break;
                            case SQLITE_INTEGER:
                                enc.writeInt(col.getInt64());
                                break;
                            case SQLITE_FLOAT:
                                enc.writeDouble(col.getDouble());
                                break;
                            case SQLITE_BLOB: {
                                if (i >= _1stCustomResultColumn) {
                                    slice fleeceData {col.getBlob(), (size_t)col.getBytes()};
                                    Scope fleeceScope(fleeceData, sk);
                                    const Value *value = Value::fromTrustedData(fleeceData);
                                    if (!value)
                                        error::_throw(error::CorruptRevisionData);
                                    enc.writeValue(value);
                                    break;
                                }
                                // else fall through:
                            case SQLITE_TEXT:
                                enc.writeString(slice{col.getText(), (size_t)col.getBytes()});
                                break;
                            }
                        }
                    }
                    enc.endArray();
                }
                _statement->reset();
            } catch (...) {
                unicodesn_tokenizerRunningQuery(false);
                throw;
            }
            unicodesn_tokenizerRunningQuery(false);

            enc.endArray();
            return enc.finishDoc();
        }

        unsigned objectRef() const                  {return getObjectRef();}   // (for logging)

        set<string> _parameters;            // Names of the bindable parameters
        vector<string> _ftsTables;          // Names of the FTS tables used
        unsigned _1stCustomResultColumn;    // Column index of the 1st column declared in JSON

    protected:
        ~SQLiteQuery() =default;
        string loggingClassName() const override    {return "Query";}

    private:
        alloc_slice _json;                                  // Original JSON form of the query
        shared_ptr<SQLite::Statement> _statement;           // Compiled SQLite statement
        unique_ptr<SQLite::Statement> _matchedTextStatement;// Gets the matched text
        vector<string> _columnTitles;                       // Titles of columns
    };


#pragma mark - QUERY ENUMERATOR:


    // Query enumerator that reads from prerecorded Fleece data (generated by fastForward(), below)
    // Each array item is a row, which is itself an array of column values.
    class SQLiteQueryEnumerator : public QueryEnumerator, protected Logging {
    public:
        SQLiteQueryEnumerator(SQLiteQuery *query,
                              const Query::Options *options,
                              sequence_t lastSequence,
                              uint64_t purgeCount)
        :QueryEnumerator(options, lastSequence, purgeCount)
        ,Logging(QueryLog)
        ,_1stCustomResultColumn(query->_1stCustomResultColumn)
        ,_hasFullText(!query->_ftsTables.empty())
        { }

        ~SQLiteQueryEnumerator() {
            logInfo("Deleted");
        }

        Array::iterator columns() const noexcept override {
            Array::iterator i(rawColumns());
            i += _1stCustomResultColumn;
            return i;
        }

        QueryEnumerator* refresh(Query *query) override {
            auto newOptions = _options.after(_lastSequence).withPurgeCount(_purgeCount);
            auto sqliteQuery = (SQLiteQuery*)query;
            unique_ptr<SQLiteQueryEnumerator> newEnum(
                (SQLiteQueryEnumerator*)sqliteQuery->createEnumerator(&newOptions) );
            if (obsoletedBy(newEnum.get())) {
                // Results have changed, so return new enumerator:
                return newEnum.release();
            }
            return nullptr;
        }

        bool hasFullText() const override {
            return _hasFullText;
        }

        const FullTextTerms& fullTextTerms() override {
            _fullTextTerms.clear();
            const Array *cols = rawColumns();
            uint64_t dataSource = cols->get(kFTSRowidCol)->asInt();
            // The offsets() function returns a string of space-separated numbers in groups of 4.
            string offsets = cols->get(kFTSOffsetsCol)->asString().asString();
            const char *termStr = offsets.c_str();
            while (*termStr) {
                uint32_t n[4];
                for (int i = 0; i < 4; ++i) {
                    char *next;
                    n[i] = (uint32_t)strtol(termStr, &next, 10);
                    termStr = next;
                }
                _fullTextTerms.push_back({dataSource, n[0], n[1], n[2], n[3]});
                // {rowid, key #, term #, byte offset, byte length}
            }
            return _fullTextTerms;
        }

    protected:
        string loggingClassName() const override    {return "QueryEnum";}
        virtual const Array* rawColumns() const = 0;

        unsigned _1stCustomResultColumn;    // Column index of the 1st column declared in JSON
        bool _hasFullText;
        bool _first {true};
    };



#pragma mark - PLAYBACK ENUMERATOR:


    class SQLiteQueryPlaybackEnum : public SQLiteQueryEnumerator {
    public:
        SQLiteQueryPlaybackEnum(SQLiteQuery *query,
                                const Query::Options *options,
                                sequence_t lastSequence,
                                uint64_t purgeCount,
                                Doc *recording,
                                int64_t firstRow,
                                double elapsedTime)
        :SQLiteQueryEnumerator(query, options, lastSequence, purgeCount)
        ,_recording(recording)
        ,_firstRow(firstRow)
        ,_iter(_recording->asArray())
        {
            logInfo("Created on {Query#%u} with %u rows from %lld (%zu bytes) in %.3fms",
                    query->objectRef(), _iter.count(), firstRow,
                    recording->data().size, elapsedTime*1000);
        }

        virtual int64_t getRowCount() const override {
            return _firstRow + _recording->asArray()->count();
        }

        virtual void seek(int64_t rowIndex) override {
            rowIndex -= _firstRow;
            auto rows = _recording->asArray();
            if (rowIndex == -1) {
                rowIndex = 0;
                _first = true;
            } else if (rowIndex >= 0 && rowIndex < rows->count()) {
                _first = false;
            } else {
                error::_throw(error::InvalidParameter);
            }
            _iter = Array::iterator(rows);
            _iter += (uint32_t)rowIndex;
        }

        bool next() override {
            if (_first)
                _first = false;
            else
                _iter += 1;
            if (!_iter) {
                logVerbose("END");
                return false;
            }
            if (willLog(LogLevel::Verbose)) {
                alloc_slice json = _iter->asArray()->toJSON();
                logVerbose("--> %.*s", SPLAT(json));
            }
            return true;
        }

        virtual bool obsoletedBy(const QueryEnumerator *otherE) override {
            if (!otherE)
                return false;
            auto other = dynamic_cast<const SQLiteQueryPlaybackEnum*>(otherE);
            if(!other || other->purgeCount() != _purgeCount) {
                // If other is null for some weird reason all bets are off.  Otherwise
                // a purge will make changes that are unrecognizable to either lastSequence
                // or the data doc, so don't consult them
                return true;
            }

            if (other->lastSequence() <= _lastSequence) {
                return false;
            } else if (_recording->data() == other->_recording->data()) {
                _lastSequence = (sequence_t)other->_lastSequence;
                _purgeCount = (uint64_t)other->_purgeCount;
                return false;
            } else {
                return true;
            }
        }

    //protected:
        virtual const Array* rawColumns() const override {
            return _iter.value()->asArray();
        }

    private:
        Retained<Doc> _recording;
        Array::iterator _iter;
        int64_t _firstRow;
    };



#pragma mark - ONE-SHOT ENUMERATOR:


    class SQLiteQueryOneShotEnum : public SQLiteQueryEnumerator,
                                   public DataFile::PreTransactionObserver
    {
    public:
        SQLiteQueryOneShotEnum(SQLiteQuery *query,
                               const Query::Options *options,
                               sequence_t lastSequence,
                               uint64_t purgeCount,
                               shared_ptr<SQLite::Statement> statement)
        :SQLiteQueryEnumerator(query, options, lastSequence, purgeCount)
        ,_query(query)
        ,_statement(statement)
        ,_sk(query->keyStore().dataFile().documentKeys())
        {
            logInfo("Created one-shot enum on {Query#%u}", query->objectRef());

            // Observe a transaction starting, so I can finish reading the rest of the result
            // rows before the database changes out from under me.
            query->keyStore().dataFile().addPreTransactionObserver(this);
            _observingTransaction = true;
        }

        ~SQLiteQueryOneShotEnum() {
            try {
                endObservingTransaction();
                if (_statement)
                    _statement->reset();
            } catch (...) { }
        }

        bool next() override {
            _encodedRow = nullslice;
            _rawColumns = nullptr;
            _hasRow = false;

            if (_recording) {
                return _recording->next();
            } else {
                if (_statement) {
                    ++_rowNumber;
                    if (_statement->executeStep())
                        _hasRow = true;
                    else {
                        // Reached end of result set:
                        _statement->reset();
                        _statement = nullptr;
                        endObservingTransaction();
                    }
                }
                return _hasRow;
            }
        }

        virtual bool obsoletedBy(const QueryEnumerator *otherE) override {
            if (_recording)
                return _recording->obsoletedBy(otherE);

            return true; //FIX
        }

        void preTransaction() override {
            // My SQLite connection is beginning a transaction, so it's probably going to make
            // changes to the database. I have to finish running the query right now, otherwise
            // I may get rows altered by the current changes. So I create a recording.
            _observingTransaction = false;
            if (_statement) {
                logInfo("Recording rest of query rows before DB changes...");
                _recording = fastForward();
                _statement->reset();
                _statement = nullptr;
            }
        }

        void endObservingTransaction() {
            if (_observingTransaction) {
                _observingTransaction = false;
                _query->keyStore().dataFile().removePreTransactionObserver(this);
            }
        }

    protected:
        virtual const Array* rawColumns() const override {
            if (_rawColumns)
                return _rawColumns;                 // Already encoded this row
            else if (_recording)
                return _recording->rawColumns();    // I switched to using a recording
            else if (_hasRow)
                return const_cast<SQLiteQueryOneShotEnum*>(this)->readRow();
            else
                return nullptr;
        }

    private:
        const Array* readRow() {
            Assert(_statement);
            unicodesn_tokenizerRunningQuery(true);
            try {
                auto nCols = _statement->getColumnCount();
                _enc.beginArray(nCols);
                for (int i = 0; i < nCols; ++i)
                encodeColumn(_enc, i);
                _enc.endArray();
            } catch (...) {
                _enc.reset();
                unicodesn_tokenizerRunningQuery(false);
                throw;
            }
            unicodesn_tokenizerRunningQuery(false);

            _encodedRow = _enc.finish();
            _rawColumns = Value::fromTrustedData(_encodedRow)->asArray();
            return _rawColumns;
        }

        void encodeColumn(Encoder &enc, int i) {
            SQLite::Column col = _statement->getColumn(i);
            switch (col.getType()) {
                case SQLITE_NULL:
                    enc.writeUndefined();
                    break;
                case SQLITE_INTEGER:
                    enc.writeInt(col.getInt64());
                    break;
                case SQLITE_FLOAT:
                    enc.writeDouble(col.getDouble());
                    break;
                case SQLITE_BLOB: {
                    if (i >= _1stCustomResultColumn) {
                        slice fleeceData {col.getBlob(), (size_t)col.getBytes()};
                        Scope fleeceScope(fleeceData, _sk);
                        const Value *value = Value::fromTrustedData(fleeceData);
                        if (!value)
                            error::_throw(error::CorruptRevisionData);
                        enc.writeValue(value);
                        break;
                    }
                    // else fall through:
                case SQLITE_TEXT:
                    enc.writeString(slice{col.getText(), (size_t)col.getBytes()});
                    break;
                }
            }
        }

        // Collects all the (remaining) rows into a Fleece array of arrays,
        // and returns an enumerator impl that will replay them.
        SQLiteQueryPlaybackEnum* fastForward() {
            fleece::Stopwatch st;
            Retained<Doc> recording = _query->fastForward();
            return new SQLiteQueryPlaybackEnum(_query, &_options, _lastSequence, _purgeCount,
                                               recording, _rowNumber + 1, st.elapsed());
        }

        Retained<SQLiteQuery> _query;
        shared_ptr<SQLite::Statement> _statement;
        SharedKeys* _sk;
        Encoder _enc;
        alloc_slice _encodedRow;
        const Array* _rawColumns {nullptr};
        Retained<SQLiteQueryPlaybackEnum> _recording;
        int64_t _rowNumber {0};
        bool _hasRow {false};
        bool _observingTransaction {false};
    };


#pragma mark - FACTORY METHODS:


    // The factory method that creates a SQLite Query.
    Retained<Query> SQLiteKeyStore::compileQuery(slice selectorExpression, QueryLanguage language) {
        return new SQLiteQuery(*this, selectorExpression, language);
    }


    void SQLiteQuery::bindParameters(const Options *options) {
        _statement->clearBindings();

        set<string> unboundParameters = _parameters;

        if (options && options->paramBindings.size > 0) {
            slice bindings = options->paramBindings;
            alloc_slice fleeceData;
            if (bindings[0] == '{' && bindings[bindings.size-1] == '}')
                fleeceData = JSONConverter::convertJSON(bindings);
            else
                fleeceData = bindings;
            const Dict *root = Value::fromData(fleeceData)->asDict();
            if (!root)
                error::_throw(error::InvalidParameter);

            for (Dict::iterator it(root); it; ++it) {
                auto key = (string)it.keyString();
                unboundParameters.erase(key);
                auto sqlKey = string("$_") + key;
                const Value *val = it.value();
                try {
                    switch (val->type()) {
                        case kNull:
                            break;
                        case kBoolean:
                        case kNumber:
                            if (val->isInteger() && !val->isUnsigned())
                                _statement->bind(sqlKey, (long long)val->asInt());
                            else
                                _statement->bind(sqlKey, val->asDouble());
                            break;
                        case kString:
                            _statement->bind(sqlKey, (string)val->asString());
                            break;
                        default: {
                            // Encode other types as a Fleece blob:
                            Encoder enc;
                            enc.writeValue(val);
                            alloc_slice asFleece = enc.finish();
                            _statement->bind(sqlKey, asFleece.buf, (int)asFleece.size);
                            break;
                        }
                    }
                } catch (const SQLite::Exception &x) {
                    if (x.getErrorCode() == SQLITE_RANGE)
                        error::_throw(error::InvalidQueryParam,
                                      "Unknown query property '%s'", key.c_str());
                    else
                        throw;
                }
            }
        }

        if (!unboundParameters.empty()) {
            stringstream msg;
            for (const string &param : unboundParameters)
                msg << " $" << param;
            Warn("Some query parameters were left unbound and will have value `MISSING`:%s",
                 msg.str().c_str());
        }
    }



    // The factory method that creates a SQLite QueryEnumerator, but only if the database has
    // changed since lastSeq.
    QueryEnumerator* SQLiteQuery::createEnumerator(const Options *options) {
        // Start a read-only transaction, to ensure that the result of lastSequence() and purgeCount() will be
        // consistent with the query results.
        ReadOnlyTransaction t(keyStore().dataFile());

        sequence_t curSeq = lastSequence();
        uint64_t purgeCnt = purgeCount();
        if(options && options->notOlderThan(curSeq, purgeCnt))
            return nullptr;

        Assert(_statement.use_count() == 1, "Multiple enumerators on a query");
        _statement->reset();

        bindParameters(options);
        if (options && options->oneShot) {
            return new SQLiteQueryOneShotEnum(this, options, curSeq, purgeCnt,
                                              _statement);
        } else {
            fleece::Stopwatch st;
            Retained<Doc> recording = fastForward();
            return new SQLiteQueryPlaybackEnum(this, options, curSeq, purgeCnt,
                                               recording, 0, st.elapsed());
        }
    }

}
