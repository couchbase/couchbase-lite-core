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

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Logging.hh"
#include "Query.hh"
#include "QueryParser.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Fleece.hh"
#include "Path.hh"
#include "Stopwatch.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sqlite3.h>
#include <sstream>
#include <iostream>

using namespace std;
using namespace fleece;

namespace litecore {

    class SQLiteQueryEnumerator;


    // Implicit columns in full-text query result:
    enum {
        kFTSRowidCol,
        kFTSOffsetsCol
    };


    class SQLiteQuery : public Query, Logging {
    public:
        SQLiteQuery(SQLiteKeyStore &keyStore, slice selectorExpression)
        :Query(keyStore)
        ,Logging(QueryLog)
        {
            log("Compiling JSON query: %.*s", SPLAT(selectorExpression));
            QueryParser qp(keyStore.tableName());
            qp.parseJSON(selectorExpression);

            _parameters = qp.parameters();
            for (auto p = _parameters.begin(); p != _parameters.end();) {
                if (hasPrefix(*p, "opt_"))
                    p = _parameters.erase(p);       // Optional param, don't warn if it's unbound
                else
                    ++p;
            }

            _ftsTables = qp.ftsTablesUsed();
            for (auto ftsTable : _ftsTables) {
                if (!keyStore.db().tableExists(ftsTable))
                    error::_throw(error::NoSuchIndex, "'match' test requires a full-text index");
            }

            string sql = qp.SQL();
            LogTo(SQL, "Compiled {Query#%u}: %s", _objectRef, sql.c_str());
            _statement.reset(keyStore.compile(sql));
            
            _1stCustomResultColumn = qp.firstCustomResultColumn();
            _isAggregate = qp.isAggregateQuery();
        }


        sequence_t lastSequence() const {
            return keyStore().lastSequence();
        }


        alloc_slice getMatchedText(const FullTextTerm &term) override {
            // Get the expression that generated the text
            if (_ftsTables.size() == 0)
                error::_throw(error::NoSuchIndex);
            string expr = _ftsTables[0];    // TODO: Support for multiple matches in a query

            if (!_matchedTextStatement) {
                auto &df = (SQLiteDataFile&) keyStore().dataFile();
                string sql = "SELECT * FROM \"" + expr + "\" WHERE docid=?";
                _matchedTextStatement.reset(new SQLite::Statement(df, sql));
            }

            alloc_slice matchedText;
            _matchedTextStatement->bind(1, (long long)term.dataSource); // dataSource is docid
            if (_matchedTextStatement->executeStep())
                matchedText = alloc_slice( ((SQLiteKeyStore&)keyStore()).columnAsSlice(_matchedTextStatement->getColumn(term.keyIndex)) );
            else
                Warn("FTS index %s has no row for docid %llu", expr.c_str(), term.dataSource);
            _matchedTextStatement->reset();
            return matchedText;
        }


        virtual unsigned columnCount() const noexcept override {
            return _statement->getColumnCount() - _1stCustomResultColumn;
        }


        string explain() override {
            stringstream result;
            // https://www.sqlite.org/eqp.html
            string query = _statement->getQuery();
            result << query << "\n";

            string sql = "EXPLAIN QUERY PLAN " + query;
            auto &df = (SQLiteDataFile&) keyStore().dataFile();
            SQLite::Statement x(df, sql);
            while (x.executeStep()) {
                for (int i = 0; i < 3; ++i)
                    result << x.getColumn(i).getInt() << "|";
                result << " " << x.getColumn(3).getText() << "\n";
            }
            return result.str();
        }

        virtual QueryEnumerator* createEnumerator(const Options *options) override;
        SQLiteQueryEnumerator* createEnumerator(const Options *options, sequence_t lastSeq);

        unsigned objectRef() const                  {return _objectRef;}

        set<string> _parameters;
        vector<string> _ftsTables;
        unsigned _1stCustomResultColumn;
        bool _isAggregate;

        shared_ptr<SQLite::Statement> statement() {return _statement;}

    protected:
        ~SQLiteQuery() =default;
        string loggingClassName() const override    {return "Query";}

    private:
        shared_ptr<SQLite::Statement> _statement;
        unique_ptr<SQLite::Statement> _matchedTextStatement;
    };


#pragma mark - QUERY ENUMERATOR:


    // Base class of SQLite query enumerators.
    class SQLiteQueryEnumBase {
    public:
        SQLiteQueryEnumBase(SQLiteQuery *query,
                            const Query::Options *options,
                            sequence_t lastSequence)
        :_query(query)
        ,_lastSequence(lastSequence)
        {
            if (options)
                _options = *options;
        }

    protected:
        Retained<SQLiteQuery> _query;
        Query::Options _options;
        sequence_t _lastSequence;       // DB's lastSequence at the time the query ran
    };



    // Query enumerator that reads from prerecorded Fleece data (generated by fastForward(), below)
    // Each array item is a row, which is itself an array of column values.
    class SQLiteQueryEnumerator : public QueryEnumerator, SQLiteQueryEnumBase, Logging {
    public:
        SQLiteQueryEnumerator(SQLiteQuery *query,
                              const Query::Options *options,
                              sequence_t lastSequence,
                              alloc_slice recording,
                              unsigned long long rowCount,
                              double elapsedTime)
        :SQLiteQueryEnumBase(query, options, lastSequence)
        ,Logging(QueryLog)
        ,_recording(recording)
        ,_rows(Value::fromTrustedData(_recording)->asArray())
        ,_iter(_rows)
        {
            log("Created on {Query#%u} with %llu rows (%zu bytes) in %.3fms",
                query->objectRef(), rowCount, recording.size, elapsedTime*1000);
        }

        ~SQLiteQueryEnumerator() {
            log("Deleted");
        }

        bool hasEqualContents(const SQLiteQueryEnumerator* other) const {
            return _recording == other->_recording;
        }

        virtual int64_t getRowCount() const override {
            return _rows->count() / 2;  // (every other row is a column bitmap)
        }

        virtual void seek(uint64_t rowIndex) override {
            rowIndex *= 2;
            if (rowIndex >= _rows->count())
                error::_throw(error::InvalidParameter);
            _iter = Array::iterator(_rows);
            _iter += (uint32_t)rowIndex;
            _first = false;
        }

        bool next() override {
            if (_first)
                _first = false;
            else
                _iter += 2;
            if (!_iter) {
                logVerbose("END");
                return false;
            }
            if (willLog(LogLevel::Verbose)) {
                SharedKeys* sharedKeys = _query->keyStore().dataFile().documentKeys();
                alloc_slice json = _iter->asArray()->toJSON(sharedKeys);
                logVerbose("--> %.*s", SPLAT(json));
            }
            return true;
        }

        Array::iterator columns() const noexcept override {
            Array::iterator i(_iter[0u]->asArray());
            i += _query->_1stCustomResultColumn;
            return i;
        }

        uint64_t missingColumns() const noexcept override {
            return _iter[1u]->asUnsigned();
        }


        QueryEnumerator* refresh() override {
            unique_ptr<SQLiteQueryEnumerator> newEnum(
                                            _query->createEnumerator(&_options, _lastSequence) );
            if (newEnum) {
                if (!hasEqualContents(newEnum.get())) {
                    // Results have changed, so return new enumerator:
                    return newEnum.release();
                }
                // Results have not changed, but update my lastSequence before returning null:
                _lastSequence = newEnum->_lastSequence;
            }
            return nullptr;
        }

        bool hasFullText() const override {
            return !_query->_ftsTables.empty();
        }

        const FullTextTerms& fullTextTerms() override {
            _fullTextTerms.clear();
            uint64_t dataSource = _iter->asArray()->get(kFTSRowidCol)->asInt();
            // The offsets() function returns a string of space-separated numbers in groups of 4.
            string offsets = _iter->asArray()->get(kFTSOffsetsCol)->asString().asString();
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

    private:
        alloc_slice _recording;
        const Array* _rows;
        Array::iterator _iter;
        bool _first {true};
    };



    // Reads from 'live' SQLite statement and records the results into a Fleece array,
    // which is then used as the data source of a SQLiteQueryEnum.
    class SQLiteQueryRunner : public SQLiteQueryEnumBase {
    public:
        SQLiteQueryRunner(SQLiteQuery *query, const Query::Options *options, sequence_t lastSequence)
        :SQLiteQueryEnumBase(query, options, lastSequence)
        ,_statement(query->statement())
        {
            _statement->clearBindings();
            _unboundParameters = _query->_parameters;
            if (options && options->paramBindings.buf)
                bindParameters(options->paramBindings);
            if (!_unboundParameters.empty()) {
                stringstream msg;
                for (const string &param : _unboundParameters)
                    msg << " $" << param;
                Warn("Some query parameters were left unbound and will have value `MISSING`:%s",
                     msg.str().c_str());
            }

            LogStatement(*_statement);
        }

        ~SQLiteQueryRunner() {
            try {
                _statement->reset();
            } catch (...) { }
        }

        void bindParameters(slice json) {
            alloc_slice fleeceData;
            fleeceData = JSONConverter::convertJSON(json);
            const Dict *root = Value::fromData(fleeceData)->asDict();
            if (!root)
                error::_throw(error::InvalidParameter);
            for (Dict::iterator it(root); it; ++it) {
                auto key = (string)it.key()->asString();
                _unboundParameters.erase(key);
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
                        case kData: {
                            slice str = val->asString();
                            _statement->bind(sqlKey, str.buf, (int)str.size);
                            break;
                        }
                        default:
                            error::_throw(error::InvalidParameter);
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

        bool encodeColumn(Encoder &enc, int i) {
            SQLite::Column col = _statement->getColumn(i);
            switch (col.getType()) {
                case SQLITE_NULL:
                    enc.writeNull();
                    return false;   // this column value is missing
                case SQLITE_INTEGER:
                    enc.writeInt(col.getInt64());
                    break;
                case SQLITE_FLOAT:
                    enc.writeDouble(col.getDouble());
                    break;
                case SQLITE_BLOB: {
                    if (i >= _query->_1stCustomResultColumn) {
                        slice fleeceData {col.getBlob(), (size_t)col.getBytes()};
                        const Value *value = Value::fromData(fleeceData);
                        if (!value)
                            error::_throw(error::CorruptData);
                        enc.writeValue(value);
                        break;
                    }
                    // else fall through:
                case SQLITE_TEXT:
                    enc.writeString(slice{col.getText(), (size_t)col.getBytes()});
                    break;
                }
            }
            return true;
        }

        // Collects all the (remaining) rows into a Fleece array of arrays,
        // and returns an enumerator impl that will replay them.
        SQLiteQueryEnumerator* fastForward() {
            Stopwatch st;
            int nCols = _statement->getColumnCount();
            uint64_t rowCount = 0;
            Encoder enc;
            enc.beginArray();
            while (_statement->executeStep()) {
                uint64_t missingCols = 0;
                enc.beginArray(nCols);
                for (int i = 0; i < nCols; ++i) {
                    if (!encodeColumn(enc, i) && i < 64)
                        missingCols |= (1 << i);
                }
                enc.endArray();
                // Add an integer containing a bit-map of which columns are missing/undefined:
                enc.writeUInt(missingCols);
                ++rowCount;
            }
            enc.endArray();
            alloc_slice recording = enc.extractOutput();
            return new SQLiteQueryEnumerator(_query, &_options, _lastSequence, recording,
                                             rowCount, st.elapsed());
        }

    private:
        shared_ptr<SQLite::Statement> _statement;
        set<string> _unboundParameters;
    };



    // The factory method that creates a SQLite Query.
    Retained<Query> SQLiteKeyStore::compileQuery(slice selectorExpression) {
        return new SQLiteQuery(*this, selectorExpression);
    }


    // The factory method that creates a SQLite QueryEnumerator, but only if the database has
    // changed since lastSeq.
    SQLiteQueryEnumerator* SQLiteQuery::createEnumerator(const Options *options,
                                                         sequence_t lastSeq)
    {
        // Start a read-only transaction, to ensure that the result of lastSequence() will be
        // consistent with the query results.
        ReadOnlyTransaction t(keyStore().dataFile());

        sequence_t curSeq = lastSequence();
        if (lastSeq > 0 && lastSeq == curSeq)
            return nullptr;
        SQLiteQueryRunner recorder(this, options, curSeq);
        return recorder.fastForward();
    }

    QueryEnumerator* SQLiteQuery::createEnumerator(const Options *options) {
        return createEnumerator(options, 0);
    }

}
