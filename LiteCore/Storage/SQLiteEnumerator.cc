//
// SQLiteEnumerator.cc
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
            rec.updateSequence((int64_t)_stmt->getColumn(RecordColumn::Sequence));
            rec.setKey(SQLiteKeyStore::columnAsSlice(_stmt->getColumn(RecordColumn::Key)));
            rec.setExpiration(_stmt->getColumn(RecordColumn::Expiration));
            SQLiteKeyStore::setRecordMetaAndBody(rec, *_stmt, _content);
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
            LogToAt(QueryLog, Debug, "%s", sql.str().c_str());
        }


        if (bySequence)
            stmt->bind(1, (long long)since);
        return new SQLiteEnumerator(stmt, options.contentOption);
    }

}
