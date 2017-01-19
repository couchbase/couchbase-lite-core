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
        kMetaCol,
        kFTSOffsetsCol,     // only if there is a MATCH expression
    };


    class SQLiteQuery : public Query {
    public:
        SQLiteQuery(SQLiteKeyStore &keyStore, slice selectorExpression)
        :Query(keyStore)
        {
            QueryParser qp(keyStore.tableName());
            qp.setBaseResultColumns({"sequence", "key", "meta"});
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
        }


        alloc_slice getMatchedText(slice recordID, sequence_t seq) override {
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
                    auto root = fleece::Value::fromTrustedData(rec.body());
                    //TODO: Support multiple FTS properties in a query
                    auto textObj = fleece::Path::eval(path, nullptr, root);
                    if (textObj)
                        result = textObj->asString();
                }
            });
            return result;
        }
        
        vector<string> _ftsTables;
        unsigned _1stCustomResultColumn;
        
        shared_ptr<SQLite::Statement> statement() {return _statement;}

    protected:
        QueryEnumerator::Impl* createEnumerator(const QueryEnumerator::Options *options) override;

    private:
        shared_ptr<SQLite::Statement> _statement;
    };


#pragma mark - QUERY ENUMERATOR:


    class SQLiteQueryEnumImpl : public QueryEnumerator::Impl {
    public:
        SQLiteQueryEnumImpl(SQLiteQuery &query, const QueryEnumerator::Options *options)
        :_query(query)
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
                string key = string("$_") + (string)it.key()->asString();
                const Value *val = it.value();
                try {
                    switch (val->type()) {
                        case kNull:
                            break;
                        case kBoolean:
                        case kNumber:
                            if (val->isInteger() && !val->isUnsigned())
                                _statement->bind(key, (long long)val->asInt());
                            else
                                _statement->bind(key, val->asDouble());
                            break;
                        case kString:
                            _statement->bind(key, (string)val->asString());
                            break;
                        case kData: {
                            slice str = val->asString();
                            _statement->bind(key, str.buf, (int)str.size);
                            break;
                        }
                        default:
                            error::_throw(error::InvalidParameter);
                    }
                } catch (const SQLite::Exception &x) {
                    if (x.getErrorCode() == SQLITE_RANGE)
                        error::_throw(error::InvalidQueryParam);
                }
            }
        }

        bool next(slice &outRecordID, sequence_t &outSequence) override {
            if (!_statement->executeStep())
                return false;
            outSequence = (int64_t)_statement->getColumn(kSeqCol);
            outRecordID = recordID();
            return true;
        }

        slice recordID() {
            return {(const void*)_statement->getColumn(kDocIDCol),
                    (size_t)_statement->getColumn(kDocIDCol).size()};
        }

        slice meta() override {
            return {_statement->getColumn(kMetaCol),
                    (size_t)_statement->getColumn(kMetaCol).size()};
        }
        
        bool hasFullText() override {
            return !_query._ftsTables.empty();
        }

        void getFullTextTerms(std::vector<QueryEnumerator::FullTextTerm>& terms) override {
            terms.clear();
            // The offsets() function returns a string of space-separated numbers in groups of 4.
            const char *str = _statement->getColumn(kFTSOffsetsCol);
            while (*str) {
                uint32_t n[4];
                for (int i = 0; i < 4; ++i) {
                    char *next;
                    n[i] = (uint32_t)strtol(str, &next, 10);
                    str = next;
                }
                terms.push_back({n[1], n[2], n[3]});    // {term #, byte offset, byte length}
            }
         }

        alloc_slice getMatchedText() override {
            return _query.getMatchedText(recordID(), (int64_t)_statement->getColumn(kSeqCol));
        }

        alloc_slice getCustomColumns() override {
            int nCols = _statement->getColumnCount();
            if (_query._1stCustomResultColumn >= nCols)
                return alloc_slice();

            Encoder enc;
            enc.beginArray();
            for (int i = _query._1stCustomResultColumn; i < nCols; ++i) {
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
                    case SQLITE_TEXT:
                        enc.writeString(slice{col.getText(), (size_t)col.getBytes()});
                        break;
                    case SQLITE_BLOB:
                        error::_throw(error::UnsupportedOperation);    //TODO: Array/dict value??
                        break;
                }
            }
            enc.endArray();
            return enc.extractOutput();
        }

    private:
        SQLiteQuery &_query;
        shared_ptr<SQLite::Statement> _statement;
    };


    QueryEnumerator::Impl* SQLiteQuery::createEnumerator(const QueryEnumerator::Options *options) {
        return new SQLiteQueryEnumImpl(*this, options);
    }


    Query* SQLiteKeyStore::compileQuery(slice selectorExpression) {
        ((SQLiteDataFile&)dataFile()).registerFleeceFunctions();
        return new SQLiteQuery(*this, selectorExpression);
    }

}
