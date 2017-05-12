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


    // Default columns in query result
    enum {
        kSeqCol = 0,
        kDocIDCol,
        kVersionCol,
        kFlagsCol,
        kFTSOffsetsCol,     // only if there is a MATCH expression
    };


    class SQLiteQuery : public Query {
    public:
        SQLiteQuery(SQLiteKeyStore &keyStore, slice selectorExpression)
        :Query(keyStore)
        {
            QueryParser qp(keyStore.tableName());
            qp.setBaseResultColumns({"sequence", "key", "version", "flags"});
            qp.setDefaultOffset("$offset");
            qp.setDefaultLimit("$limit");
            qp.parseJSON(selectorExpression);

            string sql = qp.SQL();
            LogTo(SQL, "Compiled Query: %s", sql.c_str());
            _statement.reset(keyStore.compile(sql));
            
            _ftsTables = qp.ftsTablesUsed();
            for (auto ftsTable : _ftsTables) {
                if (!keyStore.db().tableExists(ftsTable))
                    error::_throw(error::LiteCore, error::NoSuchIndex);
            }
            _1stCustomResultColumn = qp.firstCustomResultColumn();
            _isAggregate = qp.isAggregateQuery();
        }


        alloc_slice getMatchedText(slice recordID, sequence_t seq) override {
            if (!recordID || seq == 0)
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
            keyStore().get(recordID, kDefaultContent, [&](const Record &rec) {
                if (rec.body() && rec.sequence() == seq) {
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

        vector<string> _ftsTables;
        unsigned _1stCustomResultColumn;
        bool _isAggregate;

        shared_ptr<SQLite::Statement> statement() {return _statement;}

    protected:
        QueryEnumerator::Impl* createEnumerator(const QueryEnumerator::Options *options) override;

    private:
        shared_ptr<SQLite::Statement> _statement;
    };


#pragma mark - QUERY ENUMERATOR:


    // Base class of SQLite query enumerators.
    class SQLiteBaseQueryEnumImpl : public QueryEnumerator::Impl {
    public:
        SQLiteBaseQueryEnumImpl(SQLiteQuery &query)
        :_query(query)
        { }

        virtual int columnCount() =0;
        virtual slice getStringColumn(int col) =0;
        virtual int getIntColumn(int col) =0;

        slice recordID()        {return _query._isAggregate ? nullslice : getStringColumn(kDocIDCol);}
        slice version() override   {return _query._isAggregate ? nullslice : getStringColumn(kVersionCol);}
        DocumentFlags flags() override {return _query._isAggregate ? DocumentFlags::kNone : (DocumentFlags)getIntColumn(kFlagsCol);}

        virtual sequence_t sequence() =0;

        bool hasFullText() override {
            return !_query._ftsTables.empty();
        }

        void getFullTextTerms(std::vector<QueryEnumerator::FullTextTerm>& terms) override {
            terms.clear();
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
                terms.push_back({n[1], n[2], n[3]});    // {term #, byte offset, byte length}
            }
        }

        alloc_slice getMatchedText() override {
            return _query.getMatchedText(recordID(), sequence());
        }

    protected:
        SQLiteQuery &_query;
    };



    // Query enumerator that reads from prerecorded Fleece data (generated by fastForward(), below)
    // Each array item is a row, which is itself an array of column values.
    class SQLitePrerecordedQueryEnumImpl : public SQLiteBaseQueryEnumImpl {
    public:
        SQLitePrerecordedQueryEnumImpl(SQLiteQuery &query, alloc_slice recording)
        :SQLiteBaseQueryEnumImpl(query)
        ,_recording(recording)
        ,_iter(Value::fromTrustedData(_recording)->asArray())
        { }

        bool next(slice &outRecordID, sequence_t &outSequence) override {
            if (_first)
                _first = false;
            else
                ++_iter;
            if (!_iter)
                return false;
            outRecordID = recordID();
            outSequence = sequence();
            return true;
        }

        int columnCount() override {
            return _iter->asArray()->count();
        }

        slice getStringColumn(int col) override {
            return _iter->asArray()->get(col)->asString();
        }

        int getIntColumn(int col) override {
            return (int)_iter->asArray()->get(col)->asInt();
        }

        sequence_t sequence() override {
            return _query._isAggregate ? 0 : _iter->asArray()->get(kSeqCol)->asInt();
        }

        Array::iterator columns() noexcept override {
            Array::iterator i(_iter->asArray());
            i += _query._1stCustomResultColumn;
            return i;
        }

    private:
        alloc_slice _recording;
        Array::iterator _iter;
        bool _first {true};
    };



    // Query enumerator that reads from the 'live' SQLite statement.
    class SQLiteQueryEnumImpl : public SQLiteBaseQueryEnumImpl {
    public:
        SQLiteQueryEnumImpl(SQLiteQuery &query, const QueryEnumerator::Options *options)
        :SQLiteBaseQueryEnumImpl(query)
        ,_statement(query.statement())
        {
            _statement->clearBindings();
            long long offset = 0, limit = -1;
            if (options) {
                offset = options->skip;
                if (options->limit <= INT64_MAX)
                    limit = options->limit;
                if (options->paramBindings.buf)
                    bindParameters(options->paramBindings);
            }
            _statement->bind("$offset", offset);
            _statement->bind("$limit", limit );
            LogStatement(*_statement);
        }

        ~SQLiteQueryEnumImpl() {
            try {
                _statement->reset();
            } catch (...) { }
        }

        void bindParameters(slice json) {
            auto fleeceData = JSONConverter::convertJSON(json);
            const Dict *root = Value::fromData(fleeceData)->asDict();
            if (!root)
                error::_throw(error::InvalidParameter);
            for (Dict::iterator it(root); it; ++it) {
                auto key = (string)it.key()->asString();
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

        bool next(slice &outRecordID, sequence_t &outSequence) override {
            if (!_statement->executeStep())
                return false;
            outSequence = sequence();
            outRecordID = recordID();
            return true;
        }

        int columnCount() override {
            return _statement->getColumnCount();
        }

        slice getStringColumn(int col) override {
            return {(const void*)_statement->getColumn(col),
                (size_t)_statement->getColumn(col).size()};
        }

        int getIntColumn(int col) override {
            return _statement->getColumn(col);
        }

        sequence_t sequence() override {
            return _query._isAggregate ? 0 : (int64_t)_statement->getColumn(kSeqCol);
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
                    if (i >= _query._1stCustomResultColumn) {
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
        }

        // Encodes a range of result columns [beginCol...endCol) as an array to a Fleece encoder.
        void encodeColumns(Encoder &enc, int beginCol, int endCol) {
            enc.beginArray(endCol - beginCol);
            for (int i = beginCol; i < endCol; ++i)
                encodeColumn(enc, i);
            enc.endArray();
        }

        Array::iterator columns() noexcept override {
            throw logic_error("unimplemented");
        }

        // Collects all the (remaining) rows into a Fleece array of arrays,
        // and returns an enumerator impl that will replay them.
        SQLitePrerecordedQueryEnumImpl* fastForward() {
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
            return new SQLitePrerecordedQueryEnumImpl(_query, recording);
        }

    private:
        shared_ptr<SQLite::Statement> _statement;
    };



    // The factory method that creates a SQLite QueryEnumerator::Impl.
    QueryEnumerator::Impl* SQLiteQuery::createEnumerator(const QueryEnumerator::Options *options) {
        auto impl = new SQLiteQueryEnumImpl(*this, options);
        if (false) {
            return impl;
        } else {
            auto recording = impl->fastForward();
            delete impl;
            return recording;
        }
    }


    // The factory method that creates a SQLite Query.
    Query* SQLiteKeyStore::compileQuery(slice selectorExpression) {
        ((SQLiteDataFile&)dataFile()).registerFleeceFunctions();
        return new SQLiteQuery(*this, selectorExpression);
    }

}
