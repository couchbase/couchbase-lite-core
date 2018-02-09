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


    void SQLiteKeyStore::selectFrom(stringstream& in, RecordEnumerator::Options options) {
        in << "SELECT sequence, flags, key, version";
        if (options.contentOptions & kMetaOnly)
            in << ", length(body)";
        else
            in << ", body";
        in << " FROM kv_" << name();
    }

    void SQLiteKeyStore::writeSQLOptions(stringstream &sql, RecordEnumerator::Options options) {
        if (options.descending)
            sql << " DESC";
    }


    RecordEnumerator::Impl* SQLiteKeyStore::newEnumeratorImpl(bool bySequence,
                                                              sequence_t since,
                                                              RecordEnumerator::Options options)
    {
        if (bySequence && _db.options().writeable)
            createSequenceIndex();

        stringstream sql;
        selectFrom(sql, options);
        bool writeAnd = false;
        if (bySequence) {
            sql << " WHERE sequence > ?";
            writeAnd = true;
        } else {
            if (!options.includeDeleted || options.onlyBlobs)
                sql << " WHERE ";
        }
        if (!options.includeDeleted) {
            if (writeAnd) sql << " AND "; else writeAnd = true;
            sql << "(flags & 1) != 1";
        }
        if (options.onlyBlobs) {
            if(writeAnd) sql << " AND "; // else writeAnd = true;
            sql << "(flags & 4) != 0";
        }
        sql << (bySequence ? " ORDER BY sequence" : " ORDER BY key");
        writeSQLOptions(sql, options);

        auto stmt = new SQLite::Statement(db(), sql.str());        // TODO: Cache a statement
        if (bySequence)
            stmt->bind(1, (long long)since);
        return new SQLiteEnumerator(stmt, options.descending, options.contentOptions);
    }

}
