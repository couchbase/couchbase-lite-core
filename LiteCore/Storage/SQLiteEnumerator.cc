//
// SQLiteEnumerator.cc
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
#include "Logging.hh"
#include "RecordEnumerator.hh"
#include "Error.hh"
#include "FleeceImpl.hh"
#include "Path.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sstream>
#include <iostream>

using namespace std;
using namespace fleece;
using namespace fleece::impl;

namespace litecore {

   class SQLiteEnumerator : public RecordEnumerator::Impl {
    public:
        SQLiteEnumerator(SQLite::Statement *stmt, ContentOption content)
        :_stmt(stmt),
         _content(content)
        {
            LogTo(SQL, "Enumerator: %s", _stmt->getQuery().c_str());
        }

        virtual bool next() override {
            return _stmt->executeStep();
        }

        virtual bool read(Record &rec) override {
            rec.setExpiration(_stmt->getColumn(RecordColumn::Expiration));
            SQLiteKeyStore::setRecordMetaAndBody(rec, *_stmt, _content, true, true);
            return true;
        }

    private:
        unique_ptr<SQLite::Statement> _stmt;
        ContentOption _content;
    };


    RecordEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(bool bySequence,
                                                              sequence_t since,
                                                              RecordEnumerator::Options options)
    {
        if (_db.options().writeable) {
            if (bySequence)
                createSequenceIndex();
            if (options.onlyConflicts)
                createConflictsIndex();
            if (options.onlyBlobs)
                createBlobsIndex();
        }

        // Note: The result column order must match RecordColumn.
        stringstream sql;
        sql << "SELECT sequence, flags, key, version";
        sql << (options.contentOption >= kCurrentRevOnly ? ", body"  : ", length(body)");
        sql << (options.contentOption >= kEntireBody     ? ", extra" : ", length(extra)");
        sql << (mayHaveExpiration() ? ", expiration" : ", 0");
        sql << " FROM kv_" << name();
        
        bool writeAnd = false;
        if (bySequence) {
            sql << " WHERE sequence > ?";
            writeAnd = true;
        } else {
            if (!options.includeDeleted || options.onlyBlobs || options.onlyConflicts)
                sql << " WHERE ";
        }

        auto writeFlagTest = [&](DocumentFlags flag, const char *test) {
            if (writeAnd) sql << " AND "; else writeAnd = true;
            sql << "(flags & " << int(flag) << ") " << test;
        };
        
        if (!options.includeDeleted)
            writeFlagTest(DocumentFlags::kDeleted, "== 0");
        if (options.onlyBlobs)
            writeFlagTest(DocumentFlags::kHasAttachments, "!= 0");
        if (options.onlyConflicts)
            writeFlagTest(DocumentFlags::kConflicted, "!= 0");

        if (options.sortOption != kUnsorted) {
            sql << (bySequence ? " ORDER BY sequence" : " ORDER BY key");
            if (options.sortOption == kDescending)
                sql << " DESC";
        }

        auto sqlStr = sql.str();
        auto stmt = new SQLite::Statement(db(), sqlStr);        // TODO: Cache a statement
        LogTo(SQL, "%s", sqlStr.c_str());
        if (QueryLog.willLog(LogLevel::Debug)) {
            // https://www.sqlite.org/eqp.html
            SQLite::Statement x(db(), "EXPLAIN QUERY PLAN " + sqlStr);
            while (x.executeStep()) {
                sql << "\n\t";
                for (int i = 0; i < 3; ++i)
                    sql << x.getColumn(i).getInt() << "|";
                sql << " " << x.getColumn(3).getText();
            }
            LogDebug(QueryLog, "%s", sql.str().c_str());
        }


        if (bySequence)
            stmt->bind(1, (long long)since);
        return new SQLiteEnumerator(stmt, options.contentOption);
    }

}
