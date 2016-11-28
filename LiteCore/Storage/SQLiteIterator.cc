//
//  SQLiteIterator.cc
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
#include "RecordEnumerator.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sstream>
#include <iostream>

using namespace std;
using namespace fleece;

namespace litecore {

   class SQLiteIterator : public RecordEnumerator::Impl {
    public:
        SQLiteIterator(SQLite::Statement *stmt, bool descending, ContentOptions content)
        :_stmt(stmt),
         _content(content)
        { }

        virtual bool next() override {
            return _stmt->executeStep();
        }

        virtual bool read(Record &rec) override {
            updateDoc(rec, (int64_t)_stmt->getColumn(0), 0, (int)_stmt->getColumn(1));
            rec.setKey(SQLiteKeyStore::columnAsSlice(_stmt->getColumn(2)));
            SQLiteKeyStore::setRecordMetaAndBody(rec, *_stmt.get(), _content);
            return true;
        }

    private:
        unique_ptr<SQLite::Statement> _stmt;
        ContentOptions _content;
    };


    void SQLiteKeyStore::selectFrom(stringstream& in, const RecordEnumerator::Options &options) {
        in << "SELECT sequence, deleted, key, meta";
        if (options.contentOptions & kMetaOnly)
            in << ", length(body)";
        else
            in << ", body";
        in << " FROM kv_" << name();
    }

    void SQLiteKeyStore::writeSQLOptions(stringstream &sql, RecordEnumerator::Options &options) {
        if (options.descending)
            sql << " DESC";
        if (options.limit < UINT_MAX)
            sql << " LIMIT " << options.limit;
        if (options.skip > 0) {
            if (options.limit == UINT_MAX)
                sql << " LIMIT -1";             // OFFSET has to have a LIMIT before it
            sql << " OFFSET " << options.skip;
            options.skip = 0;                   // tells RecordEnumerator not to do skip on its own
        }
        options.limit = UINT_MAX;               // ditto for limit
    }


    // iterate by key:
    RecordEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(slice minKey, slice maxKey,
                                                           RecordEnumerator::Options &options)
    {
        stringstream sql;
        selectFrom(sql, options);
        bool noDeleted = _capabilities.softDeletes && !options.includeDeleted;
        if (minKey.buf || maxKey.buf || noDeleted) {
            sql << " WHERE ";
            bool writeAnd = false;
            if (minKey.buf) {
                sql << (options.inclusiveMin() ? "key >= ?" : "key > ?");
                writeAnd = true;
            }
            if (maxKey.buf) {
                if (writeAnd) sql << " AND "; else writeAnd = true;
                sql << (options.inclusiveMax() ? "key <= ?" : "key < ?");
            }
            if (_capabilities.softDeletes && noDeleted) {
                if (writeAnd) sql << " AND "; //else writeAnd = true;
                sql << "deleted!=1";
            }
        }
        sql << " ORDER BY key";
        writeSQLOptions(sql, options);

        auto stmt = new SQLite::Statement(db(), sql.str());    //TODO: Cache a statement
        int param = 1;
        if (minKey.buf)
            stmt->bind(param++, minKey.buf, (int)minKey.size);
        if (maxKey.buf)
            stmt->bind(param++, maxKey.buf, (int)maxKey.size);
        return new SQLiteIterator(stmt, options.descending, options.contentOptions);
    }

    // iterate by sequence:
    RecordEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(sequence min, sequence max,
                                                           RecordEnumerator::Options &options)
    {
        if (!_capabilities.sequences)
            error::_throw(error::NoSequences);

        if (!_createdSeqIndex) {
            db().execWithLock(string("CREATE UNIQUE INDEX IF NOT EXISTS kv_"+name()+"_seqs"
                                          " ON kv_"+name()+" (sequence)"));
            _createdSeqIndex = true;
        }

        stringstream sql;
        selectFrom(sql, options);
        sql << (options.inclusiveMin() ? " WHERE sequence >= ?" : " WHERE sequence > ?");
        if (max < INT64_MAX)
            sql << (options.inclusiveMax() ? " AND sequence <= ?"   : " AND sequence < ?");
        if (_capabilities.softDeletes && !options.includeDeleted)
            sql << " AND deleted!=1";
        sql << " ORDER BY sequence";
        writeSQLOptions(sql, options);

        auto st = new SQLite::Statement(db(), sql.str());        // TODO: Cache a statement
        st->bind(1, (long long)min);
        if (max < INT64_MAX)
            st->bind(2, (long long)max);
        return new SQLiteIterator(st, options.descending, options.contentOptions);
    }


#pragma mark - DB QUERIES:


    class SQLiteQuery : public Query {
    public:
        SQLiteQuery(SQLiteKeyStore &keyStore, slice selectorExpression, slice sortExpression)
        :Query(keyStore)
        {
            QueryParser qp(keyStore.tableName());
            qp.parseJSON(selectorExpression, sortExpression);

            stringstream sql;
            sql << "SELECT sequence, key, meta, length(body)";
            _ftsPaths = qp.ftsTableNames(); //FIX
            for (auto ftsTable : qp.ftsTableNames())
                sql << ", offsets(" << ftsTable << ")";
            sql << " FROM " << qp.fromClause() <<
                   " WHERE (" << qp.whereClause() << ")";

            auto orderBy = qp.orderByClause();
            if (!orderBy.empty())
                sql << " ORDER BY " << orderBy;

            sql << " LIMIT $limit OFFSET $offset";
            LogTo(SQL, "Compiled Query: %s", sql.str().c_str());
            _statement.reset(keyStore.compile(sql.str()));
        }

    protected:
        QueryEnumerator::Impl* createEnumerator(const QueryEnumerator::Options *options) override;
        
        shared_ptr<SQLite::Statement> statement() {return _statement;}

    private:
        friend class SQLiteQueryEnumImpl;

        shared_ptr<SQLite::Statement> _statement;
        vector<string> _ftsPaths;
    };


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
                string key = string(":_") + (string)it.key()->asString();
                const Value *val = it.value();
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
            }
        }

        bool next(slice &outRecordID, sequence_t &outSequence) override {
            if (!_statement->executeStep())
                return false;
            outSequence = (int64_t)_statement->getColumn(0);
            outRecordID.buf = _statement->getColumn(1);
            outRecordID.size = _statement->getColumn(1).size();
            return true;
        }

        slice meta() override {
            return {_statement->getColumn(2), (size_t)_statement->getColumn(2).size()};
        }
        
        size_t bodyLength() override {
            return (int64_t)_statement->getColumn(3);
        }

        bool hasFullText() override {
            return _statement->getColumnCount() < 5;
        }

    private:
        SQLiteQuery &_query;
        shared_ptr<SQLite::Statement> _statement;
    };


    QueryEnumerator::Impl* SQLiteQuery::createEnumerator(const QueryEnumerator::Options *options) {
        return new SQLiteQueryEnumImpl(*this, options);
    }


    Query* SQLiteKeyStore::compileQuery(slice selectorExpression, slice sortExpression) {
        ((SQLiteDataFile&)dataFile()).registerFleeceFunctions();
        return new SQLiteQuery(*this, selectorExpression, sortExpression);
    }

}
