//
//  SQLiteEnumerator.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Logging.hh"
#include "RecordEnumerator.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Path.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sstream>
#include <iostream>

using namespace std;
using namespace fleece;

namespace litecore {

   class SQLiteEnumerator : public RecordEnumerator::Impl {
    public:
        SQLiteEnumerator(SQLite::Statement *stmt, bool descending, ContentOptions content)
        :_stmt(stmt),
         _content(content)
        {
            LogVerbose(SQL, "Enumerator: %s", _stmt->getQuery().c_str());
        }

        virtual bool next() override {
            return _stmt->executeStep();
        }

        virtual bool read(Record &rec) override {
            rec.updateSequence((int64_t)_stmt->getColumn(0));
            rec.setFlags((DocumentFlags)(int)_stmt->getColumn(1));
            rec.setKey(SQLiteKeyStore::columnAsSlice(_stmt->getColumn(2)));
            SQLiteKeyStore::setRecordMetaAndBody(rec, *_stmt.get(), _content);
            return true;
        }

    private:
        unique_ptr<SQLite::Statement> _stmt;
        ContentOptions _content;
    };


    void SQLiteKeyStore::selectFrom(stringstream& in, const RecordEnumerator::Options &options) {
        in << "SELECT sequence, flags, key, version";
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
        if (minKey.buf || maxKey.buf || !options.includeDeleted) {
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
            if (!options.includeDeleted) {
                if (writeAnd) sql << " AND "; else writeAnd = true;
                sql << "(flags & 1) != 1";
            }
            if (options.onlyBlobs) {
                if(writeAnd) sql << " AND "; // else writeAnd = true;
                sql << "(flags & 4) != 0";
            }
        }
        sql << " ORDER BY key";
        writeSQLOptions(sql, options);

        auto stmt = new SQLite::Statement(db(), sql.str());    //TODO: Cache a statement
        int param = 1;
        if (minKey.buf)
            stmt->bind(param++, (const char*)minKey.buf, (int)minKey.size);
        if (maxKey.buf)
            stmt->bind(param++, (const char*)maxKey.buf, (int)maxKey.size);
        return new SQLiteEnumerator(stmt, options.descending, options.contentOptions);
    }

    // iterate by sequence:
    RecordEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(sequence_t min, sequence_t max,
                                                              RecordEnumerator::Options &options)
    {
        if (_db.options().writeable)
            createSequenceIndex();

        stringstream sql;
        selectFrom(sql, options);
        sql << (options.inclusiveMin() ? " WHERE sequence >= ?" : " WHERE sequence > ?");
        if (max < INT64_MAX)
            sql << (options.inclusiveMax() ? " AND sequence <= ?"   : " AND sequence < ?");
        if (!options.includeDeleted)
            sql << " AND (flags & 1) != 1";
        sql << " ORDER BY sequence";
        writeSQLOptions(sql, options);

        auto st = new SQLite::Statement(db(), sql.str());        // TODO: Cache a statement
        st->bind(1, (long long)min);
        if (max < INT64_MAX)
            st->bind(2, (long long)max);
        return new SQLiteEnumerator(st, options.descending, options.contentOptions);
    }

}
