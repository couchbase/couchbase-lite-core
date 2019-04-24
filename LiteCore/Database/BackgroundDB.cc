//
// BackgroundDB.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "BackgroundDB.hh"
#include "DataFile.hh"
#include "Database.hh"
#include "SequenceTracker.hh"
#include "Query.hh"
#include "make_unique.h"

namespace litecore {

    BackgroundDB::BackgroundDB(Database *db)
    :_database(db)
    ,_bgDataFile(db->dataFile()->openAnother(this))
    { }


    void BackgroundDB::_close() {
        _bgDataFile->close();
        _bgDataFile.reset();
    }


    slice BackgroundDB::fleeceAccessor(slice recordBody) const {
        return _database->fleeceAccessor(recordBody);
    }

    alloc_slice BackgroundDB::blobAccessor(const fleece::impl::Dict *dict) const {
        return _database->blobAccessor(dict);
    }


    void BackgroundDB::_inTransactionDo(Task task) {
        Transaction t(_bgDataFile);
        SequenceTracker sequenceTracker;
        sequenceTracker.beginTransaction();
        
        bool commit;
        try {
            commit = task(_bgDataFile.get(), &sequenceTracker);
        } catch (const exception &x) {
            t.abort();
            sequenceTracker.endTransaction(false);
            throw;
        }

        if (!commit) {
            t.abort();
            sequenceTracker.endTransaction(false);
            return;
        }

        t.commit();

        // Notify other Database instances of any changes:
        lock_guard<mutex> lock(sequenceTracker.mutex());
        _bgDataFile->forOtherDataFiles([&](DataFile *other) {
            auto db = dynamic_cast<Database*>(other->delegate());
            if (db)
                db->externalTransactionCommitted(sequenceTracker);
        });
        sequenceTracker.endTransaction(true);
    }


    void BackgroundDB::runQuery(Query *query, Query::Options options, RefreshQueryCallback callback) {
        refreshQuery(query, options, nullptr, callback);
    }

    void BackgroundDB::refreshQuery(QueryEnumerator *qe, RefreshQueryCallback callback) {
        refreshQuery(qe->query(), qe->options(), qe, callback);
    }

    void BackgroundDB::refreshQuery(Query *query, Query::Options options,
                                    QueryEnumerator *qe,
                                    RefreshQueryCallback callback)
    {
        enqueue(&BackgroundDB::_refreshQuery,
                query->expression(), options,
                (RefreshQueryCallback)[=](Retained<QueryEnumerator> newQE, error err) {
                    if (qe && newQE && !qe->obsoletedBy(newQE.get()))
                        newQE = nullptr;      // unchanged
                    callback(newQE, err);
                });
    }

    void BackgroundDB::_refreshQuery(alloc_slice expression,
                                     Query::Options options,
                                     RefreshQueryCallback callback)
    {
        try {
            // Have to recompile the query to get a statement in the bg database:
            Retained<Query> query = _bgDataFile->defaultKeyStore().compileQuery(expression);
            Retained<QueryEnumerator> qe( query->createEnumerator(&options) );
            callback(qe, error(error::LiteCore, 0));
        } catch (const exception &x) {
            callback(nullptr, error::convertException(x));
        }
    }

}
