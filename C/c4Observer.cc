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
#include "Database.hh"
#include "SequenceTracker.hh"

using namespace std::placeholders;


struct c4DatabaseObserver : public C4InstanceCounted {
    c4DatabaseObserver(C4Database *db, C4SequenceNumber since,
                       C4DatabaseObserverCallback callback, void *context)
    :_db(db),
     _callback(callback),
     _context(context),
     _notifier(db->sequenceTracker(),
               bind(&c4DatabaseObserver::dispatchCallback, this, _1),
               since)
    { }


    void dispatchCallback(DatabaseChangeNotifier&) {
        _callback(this, _context);
    }

    Retained<Database> _db;
    DatabaseChangeNotifier _notifier;
    C4DatabaseObserverCallback _callback;
    void *_context;
    //NOTE: Order of members is important! _notifier needs to appear after _db so that it will be
    // destructed *before* _db; this ensures that the Database's SequenceTracker is still in
    // existence when the notifier removes itself from it.
};


C4DatabaseObserver* c4dbobs_create(C4Database *db,
                                   C4DatabaseObserverCallback callback,
                                   void *context) noexcept
{
    return tryCatch<C4DatabaseObserver*>(nullptr, [&]{
        lock_guard<mutex> lock(db->sequenceTracker().mutex());
        return new c4DatabaseObserver(db, UINT64_MAX, callback, context);
    });
}


uint32_t c4dbobs_getChanges(C4DatabaseObserver *obs,
                            C4DatabaseChange outChanges[],
                            uint32_t maxChanges,
                            bool *outExternal) noexcept
{
    static_assert(sizeof(C4DatabaseChange) == sizeof(SequenceTracker::Change),
                  "C4DatabaseChange doesn't match SequenceTracker::Change");
    return tryCatch<uint32_t>(nullptr, [&]{
        lock_guard<mutex> lock(obs->_notifier.tracker.mutex());
        return (uint32_t) obs->_notifier.readChanges((SequenceTracker::Change*)outChanges,
                                                     maxChanges,
                                                     *outExternal);
    });
}


void c4dbobs_free(C4DatabaseObserver* obs) noexcept {
    if (obs) {
        Retained<Database> retainDB((Database*)obs->_db);   // keep db from being deleted too early
        lock_guard<mutex> lock(obs->_notifier.tracker.mutex());
        delete obs;
    }
}


#pragma mark - DOCUMENT OBSERVER:


struct c4DocumentObserver : public C4InstanceCounted {
    c4DocumentObserver(C4Database *db, C4Slice docID,
                       C4DocumentObserverCallback callback, void *context)
    :_db(db),
     _callback(callback),
     _context(context),
     _notifier(db->sequenceTracker(),
               docID,
               bind(&c4DocumentObserver::dispatchCallback, this, _1, _2, _3))
    { }


    void dispatchCallback(DocChangeNotifier&, slice docID, sequence_t sequence) {
        _callback(this, toc4slice(docID), sequence, _context);
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
        lock_guard<mutex> lock(db->sequenceTracker().mutex());
        return new c4DocumentObserver(db, docID, callback, context);
    });
}


void c4docobs_free(C4DocumentObserver* obs) noexcept {
    if (obs) {
        Retained<Database> db(obs->_db);        // keep db alive until obs is safely deleted
        lock_guard<mutex> lock(obs->_notifier.tracker.mutex());
        delete obs;
    }
}
