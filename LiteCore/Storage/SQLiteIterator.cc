//
//  SQLiteIterator.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "QueryParser.hh"
#include "DocEnumerator.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sstream>

using namespace std;
using namespace fleece;

namespace litecore {

   class SQLiteIterator : public DocEnumerator::Impl {
    public:
        SQLiteIterator(SQLite::Statement *stmt, bool descending, ContentOptions content)
        :_stmt(stmt),
         _content(content)
        { }

        virtual bool next() override {
            return _stmt->executeStep();
        }

        virtual bool read(Document &doc) override {
            updateDoc(doc, (int64_t)_stmt->getColumn(0), 0, (int)_stmt->getColumn(1));
            doc.setKey(SQLiteKeyStore::columnAsSlice(_stmt->getColumn(2)));
            SQLiteKeyStore::setDocMetaAndBody(doc, *_stmt.get(), _content);
            return true;
        }

    private:
        unique_ptr<SQLite::Statement> _stmt;
        ContentOptions _content;
    };


    stringstream SQLiteKeyStore::selectFrom(const DocEnumerator::Options &options) {
        stringstream sql;
        sql << "SELECT sequence, deleted, key, meta";
        if (options.contentOptions & kMetaOnly)
            sql << ", length(body)";
        else
            sql << ", body";
        sql << " FROM kv_" << name();
        return sql;
    }

    void SQLiteKeyStore::writeSQLOptions(stringstream &sql, DocEnumerator::Options &options) {
        if (options.descending)
            sql << " DESC";
        if (options.limit < UINT_MAX)
            sql << " LIMIT " << options.limit;
        if (options.skip > 0) {
            if (options.limit == UINT_MAX)
                sql << " LIMIT -1";             // OFFSET has to have a LIMIT before it
            sql << " OFFSET " << options.skip;
            options.skip = 0;                   // tells DocEnumerator not to do skip on its own
        }
        options.limit = UINT_MAX;               // ditto for limit
    }


    // iterate by key:
    DocEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(slice minKey, slice maxKey,
                                                           DocEnumerator::Options &options)
    {
        stringstream sql = selectFrom(options);
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
    DocEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(sequence min, sequence max,
                                                           DocEnumerator::Options &options)
    {
        if (!_capabilities.sequences)
            error::_throw(error::NoSequences);

        if (!_createdSeqIndex) {
            db().exec(string("CREATE UNIQUE INDEX IF NOT EXISTS kv_"+name()+"_seqs"
                                          " ON kv_"+name()+" (sequence)"));
            _createdSeqIndex = true;
        }

        stringstream sql = selectFrom(options);
        sql << (options.inclusiveMin() ? " WHERE sequence >= ?" : " WHERE sequence > ?");
        if (max < INT64_MAX)
            sql << (options.inclusiveMax() ? " AND sequence <= ?"   : " AND sequence < ?");
        if (_capabilities.softDeletes && !options.includeDeleted)
            sql << " AND deleted!=1";
        sql << " ORDER BY sequence";
        writeSQLOptions(sql, options);

        auto st = new SQLite::Statement(db(), sql.str());        // TODO: Cache a statement
        st->bind(1, (int64_t)min);
        if (max < INT64_MAX)
            st->bind(2, (int64_t)max);
        return new SQLiteIterator(st, options.descending, options.contentOptions);
    }

    // iterate by query:
    DocEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(const string &query,
                                                           DocEnumerator::Options &options)
    {
        stringstream sql = selectFrom(options);
        sql << " WHERE (";
        QueryParser::parseJSON(query, sql);
        sql << ") ORDER BY key";
        writeSQLOptions(sql, options);
        auto st = new SQLite::Statement(db(), sql.str());        // TODO: Cache a statement
        return new SQLiteIterator(st, options.descending, options.contentOptions);
    }

}
