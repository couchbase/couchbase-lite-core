//
//  SQLiteQuery.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
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

    class SQLitePrerecordedQueryEnum;


    // Implicit columns in full-text query result:
    enum {
        kFTSSeqCol,
        kFTSOffsetsCol
    };


    class SQLiteQuery : public Query {
    public:
        SQLiteQuery(SQLiteKeyStore &keyStore, slice selectorExpression)
        :Query(keyStore)
        {
            LogTo(SQL, "Compiling JSON query: %.*s", SPLAT(selectorExpression));
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
            if (!_ftsTables.empty())
                keyStore.createSequenceIndex();     // 'match' operator uses a join on the sequence

            string sql = qp.SQL();
            LogTo(SQL, "Compiled Query: %s", sql.c_str());
            _statement.reset(keyStore.compile(sql));
            
            _1stCustomResultColumn = qp.firstCustomResultColumn();
            _isAggregate = qp.isAggregateQuery();
        }


        sequence_t lastSequence() const {
            return keyStore().lastSequence();
        }


        alloc_slice getMatchedText(FullTextID ftsID) override {
            if (ftsID == 0)
                error::_throw(error::InvalidParameter);
            // Get the expression that generated the text
            if (_ftsTables.size() == 0)
                error::_throw(error::NoSuchIndex);
            string expr = _ftsTables[0];    // TODO: Support for multiple matches in a query
            auto delim = expr.find("::");
            Assert(delim != string::npos);

            // FIXME: Currently only property expressions are supported:
            if (expr[delim+2] != '.') {
                Warn("Unable to get matched text from expression %s", expr.c_str());
                error::_throw(error::Unimplemented);
            }
            string path = expr.substr(delim+3);

            // Now load the document and evaluate the expression:
            alloc_slice result;
            keyStore().get(ftsID, kDefaultContent, [&](const Record &rec) {
                if (rec.body()) {
                    slice fleeceData = rec.body();
                    auto accessor = keyStore().dataFile().fleeceAccessor();
                    if (accessor)
                        fleeceData = accessor(fleeceData);
                    auto root = fleece::Value::fromTrustedData(fleeceData);
                    //TODO: Support multiple FTS properties in a query
                    auto textObj = fleece::Path::eval(path,
                                                      keyStore().dataFile().documentKeys(),
                                                      root);
                    if (textObj)
                        result = textObj->asString();
                }
            });
            return result;
        }


        virtual unsigned columnCount() const noexcept override {
            return _statement->getColumnCount() - _1stCustomResultColumn;
        }


        virtual string nameOfColumn(unsigned col) const override {
            return _statement->getColumnName(_1stCustomResultColumn + col);
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
        SQLitePrerecordedQueryEnum* createEnumerator(const Options *options, sequence_t lastSeq);

        set<string> _parameters;
        vector<string> _ftsTables;
        unsigned _1stCustomResultColumn;
        bool _isAggregate;

        shared_ptr<SQLite::Statement> statement() {return _statement;}

    protected:
        ~SQLiteQuery() =default;
            
    private:
        shared_ptr<SQLite::Statement> _statement;
    };


#pragma mark - QUERY ENUMERATOR:


    // Base class of SQLite query enumerators.
    class SQLiteBaseQueryEnum : public QueryEnumerator {
    public:
        SQLiteBaseQueryEnum(SQLiteQuery *query,
                            const Query::Options *options,
                            sequence_t lastSequence)
        :_query(query)
        ,_lastSequence(lastSequence)
        {
            if (options)
                _options = *options;
        }

        const Query::Options* options() const                   {return &_options;}

        virtual int columnCount() const =0;
        virtual slice getStringColumn(int col) const =0;
        virtual int64_t getIntColumn(int col) const =0;

        bool hasFullText() const override {
            return !_query->_ftsTables.empty();
        }

        const std::vector<FullTextTerm>& fullTextTerms() override {
            _fullTextTerms.clear();
            // The offsets() function returns a string of space-separated numbers in groups of 4.
            string offsets = getStringColumn(kFTSOffsetsCol).asString();
            const char *termStr = offsets.c_str();
            while (*termStr) {
                uint32_t n[4];
                for (int i = 0; i < 4; ++i) {
                    char *next;
                    n[i] = (uint32_t)strtol(termStr, &next, 10);
                    termStr = next;
                }
                _fullTextTerms.push_back({n[1], n[2], n[3]});    // {term #, byte offset, byte length}
            }
            return _fullTextTerms;
        }

        Query::FullTextID fullTextID() const override {
            return getIntColumn(kFTSSeqCol);
        }

    protected:
        Retained<SQLiteQuery> _query;
        Query::Options _options;
        sequence_t _lastSequence;       // DB's lastSequence at the time the query ran
    };



    // Query enumerator that reads from prerecorded Fleece data (generated by fastForward(), below)
    // Each array item is a row, which is itself an array of column values.
    class SQLitePrerecordedQueryEnum : public SQLiteBaseQueryEnum {
    public:
        SQLitePrerecordedQueryEnum(SQLiteQuery *query,
                                   const Query::Options *options,
                                   sequence_t lastSequence,
                                   alloc_slice recording)
        :SQLiteBaseQueryEnum(query, options, lastSequence)
        ,_recording(recording)
        ,_rows(Value::fromTrustedData(_recording)->asArray())
        ,_iter(_rows)
        { }

        bool hasEqualContents(const SQLitePrerecordedQueryEnum* other) const {
            return _recording == other->_recording;
        }

        virtual int64_t getRowCount() const override {
            return _rows->count();
        }

        virtual void seek(uint64_t rowIndex) override {
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
                ++_iter;
            if (!_iter)
                return false;
            return true;
        }

        int columnCount() const override {
            return _iter->asArray()->count();
        }

        slice getStringColumn(int col) const override {
            return _iter->asArray()->get(col)->asString();
        }

        int64_t getIntColumn(int col) const override {
            return (int)_iter->asArray()->get(col)->asInt();
        }

        Array::iterator columns() const noexcept override {
            Array::iterator i(_iter->asArray());
            i += _query->_1stCustomResultColumn;
            return i;
        }


        QueryEnumerator* refresh() override {
            unique_ptr<SQLitePrerecordedQueryEnum> newEnum(
                                            _query->createEnumerator(options(), _lastSequence) );
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

    private:
        Query::Options _options;
        alloc_slice _recording;
        const Array* _rows;
        Array::iterator _iter;
        bool _first {true};
    };



    // Query enumerator that reads from the 'live' SQLite statement.
    class SQLiteQueryEnum : public SQLiteBaseQueryEnum {
    public:
        SQLiteQueryEnum(SQLiteQuery *query, const Query::Options *options, sequence_t lastSequence)
        :SQLiteBaseQueryEnum(query, options, lastSequence)
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

        ~SQLiteQueryEnum() {
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

        bool next() override {
            return _statement->executeStep();
        }

        int columnCount() const override {
            return _statement->getColumnCount();
        }

        slice getStringColumn(int col) const override {
            return {(const void*)_statement->getColumn(col),
                (size_t)_statement->getColumn(col).size()};
        }

        int64_t getIntColumn(int col) const override {
            return _statement->getColumn(col);
        }

        void encodeColumn(Encoder &enc, int i) {
            SQLite::Column col = _statement->getColumn(i);
            switch (col.getType()) {
                case SQLITE_NULL:
                    enc.writeNull();
                    break;
                case SQLITE_INTEGER:
                    enc.writeInt(col.getInt64());
                    break;
                case SQLITE_FLOAT:
                    enc.writeDouble(col.getDouble());
                    break;
                case SQLITE_BLOB: {
                    if (i >= _query->_1stCustomResultColumn) {
                        slice fleeceData {col.getBlob(), (size_t)col.getBytes()};
                        if (fleeceData.size == 0) {
                            // An empty SQL blob represents a Fleece/JSON null
                            enc.writeNull();
                        } else {
                            const Value *value = Value::fromData(fleeceData);
                            if (!value)
                                error::_throw(error::CorruptData);
                            enc.writeValue(value);
                        }
                        break;
                    }
                    // else fall through:
                case SQLITE_TEXT:
                    enc.writeString(slice{col.getText(), (size_t)col.getBytes()});
                    break;
                }
            }
        }

        // Encodes a range of result columns [beginCol...endCol) as an array to a Fleece encoder.
        void encodeColumns(Encoder &enc, int beginCol, int endCol) {
            enc.beginArray(endCol - beginCol);
            for (int i = beginCol; i < endCol; ++i)
                encodeColumn(enc, i);
            enc.endArray();
        }

        Array::iterator columns() const noexcept override {
            throw logic_error("unimplemented");
        }

        QueryEnumerator* refresh() override {
            throw logic_error("unimplemented");
        }

        // Collects all the (remaining) rows into a Fleece array of arrays,
        // and returns an enumerator impl that will replay them.
        SQLitePrerecordedQueryEnum* fastForward() {
            Stopwatch st;
            int nCols = _statement->getColumnCount();
            uint64_t rowCount = 0;
            Encoder enc;
            enc.beginArray(nCols);
            while (_statement->executeStep()) {
                encodeColumns(enc, 0, nCols);
                ++rowCount;
            }
            enc.endArray();
            alloc_slice recording = enc.extractOutput();
            LogTo(SQL, "Created prerecorded query enum with %llu rows (%zu bytes) in %.3fms",
                  (unsigned long long)rowCount, recording.size, st.elapsed()*1000);
            return new SQLitePrerecordedQueryEnum(_query, &_options, _lastSequence, recording);
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
    SQLitePrerecordedQueryEnum* SQLiteQuery::createEnumerator(const Options *options,
                                                              sequence_t lastSeq)
    {
        // Start a read-only transaction, to ensure that the result of lastSequence() will be
        // consistent with the query results.
        ReadOnlyTransaction t(keyStore().dataFile());

        sequence_t curSeq = lastSequence();
        if (lastSeq > 0 && lastSeq == curSeq)
            return nullptr;
        unique_ptr<SQLiteQueryEnum> newEnum(new SQLiteQueryEnum(this, options, curSeq));
        return newEnum->fastForward();
    }

    QueryEnumerator* SQLiteQuery::createEnumerator(const Options *options) {
        return createEnumerator(options, 0);
    }

}
