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


    void BackgroundDB::_doTask(Task task) {
        task(_bgDataFile.get());
    }

    void BackgroundDB::_inTransactionDo(TransactionTask task) {
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




    BackgroundQuerier::BackgroundQuerier(c4Internal::Database *db, Query *query)
    :_backgroundDB(db->backgroundDatabase())
    ,_expression(query->expression())
    ,_language(query->language())
    { }


    void BackgroundQuerier::run(Query::Options options, Callback callback) {
        run(options, nullptr, callback);
    }

    void BackgroundQuerier::refresh(QueryEnumerator *qe, Callback callback) {
        run(qe->options(), retained(qe), callback);
    }

    void BackgroundQuerier::run(Query::Options options,
                                Retained<QueryEnumerator> qe,
                                Callback callback)
    {
        _backgroundDB->doTask([=](DataFile *df) {
            // ---- running on the background thread ----
            try {
                // Create my own Query object associated with the Backgrounder's DataFile:
                if (!_query) {
                    _query = df->defaultKeyStore().compileQuery(_expression, _language);
                    _expression = nullslice;
                }

                Retained<QueryEnumerator> newQE( _query->createEnumerator(&options) );
                if (qe && newQE && !qe->obsoletedBy(newQE.get()))
                    newQE = nullptr;      // unchanged
                callback(newQE, error(error::LiteCore, 0));

            } catch (const exception &x) {
                callback(nullptr, error::convertException(x));
            }
        });
    }

}
