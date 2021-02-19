//
// c4Observer.cc
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

#include "c4Internal.hh"
#include "c4Observer.h"
#include "c4Database.hh"
#include "SequenceTracker.hh"
#include "InstanceCounted.hh"

using namespace std::placeholders;
using namespace std;


struct C4DatabaseObserver : public fleece::InstanceCounted {
    C4DatabaseObserver(C4Database *db,
                        SequenceTracker &sequenceTracker,
                        C4SequenceNumber since,
                        C4DatabaseObserverCallback callback, void *context)
    :_db(db),
     _callback(callback),
     _context(context),
     _notifier(sequenceTracker,
               bind(&C4DatabaseObserver::dispatchCallback, this, _1),
               since)
    { }


    void dispatchCallback(DatabaseChangeNotifier&) {
        _inCallback = true;
        _callback(this, _context);
        _inCallback = false;
    }

    Retained<Database> _db;
    DatabaseChangeNotifier _notifier;
    C4DatabaseObserverCallback _callback;
    void *_context;
    bool _inCallback {false};
    //NOTE: Order of members is important! _notifier needs to appear after _db so that it will be
    // destructed *before* _db; this ensures that the Database's SequenceTracker is still in
    // existence when the notifier removes itself from it.
};


C4DatabaseObserver* c4dbobs_create(C4Database *db,
                                   C4DatabaseObserverCallback callback,
                                   void *context) noexcept
{
    return tryCatch<C4DatabaseObserver*>(nullptr, [&]{
        return db->sequenceTracker().use<C4DatabaseObserver*>([&](SequenceTracker &st) {
            return new C4DatabaseObserver(db, st, UINT64_MAX, callback, context);
        });
    });
}


uint32_t c4dbobs_getChanges(C4DatabaseObserver *obs,
                            C4DatabaseChange outChanges[],
                            uint32_t maxChanges,
                            bool *outExternal) noexcept
{
    static_assert(sizeof(C4DatabaseChange) == sizeof(SequenceTracker::Change),
                  "C4DatabaseChange doesn't match SequenceTracker::Change");
    memset(outChanges, 0, maxChanges * sizeof(C4DatabaseChange));
    return tryCatch<uint32_t>(nullptr, [&]{
        return obs->_db->sequenceTracker().use<uint32_t>([&](SequenceTracker &st) {
            return (uint32_t) obs->_notifier.readChanges((SequenceTracker::Change*)outChanges,
                                                     maxChanges,
                                                     *outExternal);
            // This is slightly sketchy because SequenceTracker::Change contains alloc_slices, whereas
            // C4DatabaseChange contains slices. The result is that the docID and revID memory will be
            // temporarily leaked, since the alloc_slice destructors won't be called.
            // For this purpose we have c4dbobs_releaseChanges(), which does the same sleight of hand
            // on the array but explicitly destructs each Change object, ensuring its alloc_slices are
            // destructed and the backing store's ref-count goes back to what it was originally.
        });
    });
}


void c4dbobs_releaseChanges(C4DatabaseChange changes[], uint32_t numChanges) noexcept {
    for (uint32_t i = 0; i < numChanges; ++i) {
        auto &change = (SequenceTracker::Change&)changes[i];
        change.~Change();
    }
}


void c4dbobs_free(C4DatabaseObserver* obs) noexcept {
    if (obs) {
        Retained<Database> retainDB((Database*)obs->_db);   // keep db from being deleted too early
        retainDB->sequenceTracker().use([&](SequenceTracker &st) {
            delete obs;
        });
    }
}


#pragma mark - DOCUMENT OBSERVER:


struct C4DocumentObserver : public fleece::InstanceCounted {
    C4DocumentObserver(C4Database *db,
                        SequenceTracker &sequenceTracker,
                        C4Slice docID,
                        C4DocumentObserverCallback callback,
                        void *context)
    :_db(db),
     _callback(callback),
     _context(context),
     _notifier(sequenceTracker,
               docID,
               bind(&C4DocumentObserver::dispatchCallback, this, _1, _2, _3))
    { }


    void dispatchCallback(DocChangeNotifier&, slice docID, sequence_t sequence) {
        _callback(this, docID, sequence, _context);
    }

    Retained<Database> _db;
    C4DocumentObserverCallback _callback;
    void *_context;
    DocChangeNotifier _notifier;
    //NOTE: Order of member variables is important here too (see above).
};


C4DocumentObserver* c4docobs_create(C4Database *db,
                                    C4Slice docID,
                                    C4DocumentObserverCallback callback,
                                    void *context) noexcept
{
    return tryCatch<C4DocumentObserver*>(nullptr, [&]{
        return db->sequenceTracker().use<C4DocumentObserver*>([&](SequenceTracker &st) {
            return new C4DocumentObserver(db, st, docID, callback, context);
        });
    });
}


void c4docobs_free(C4DocumentObserver* obs) noexcept {
    if (obs) {
        Retained<Database> retainDB(obs->_db);        // keep db alive until obs is safely deleted
        retainDB->sequenceTracker().use([&](SequenceTracker &st) {
            delete obs;
        });
    }
}
