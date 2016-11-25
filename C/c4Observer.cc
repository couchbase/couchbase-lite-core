//
//  c4Observer.cc
//  LiteCore
//
//  Created by Jens Alfke on 11/4/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Internal.hh"
#include "c4Observer.h"
#include "Database.hh"
#include "SequenceTracker.hh"

using namespace std::placeholders;


struct c4DatabaseObserver : public InstanceCounted {
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
        WITH_LOCK(db);
        lock_guard<mutex> lock(db->sequenceTracker().mutex());
        return new c4DatabaseObserver(db, UINT64_MAX, callback, context);
    });
}


uint32_t c4dbobs_getChanges(C4DatabaseObserver *obs,
                            C4Slice outDocIDs[],
                            uint32_t maxChanges,
                            C4SequenceNumber* outLastSequence,
                            bool *outExternal) noexcept
{
    return tryCatch<uint32_t>(nullptr, [&]{
        lock_guard<mutex> lock(obs->_notifier.tracker.mutex());
        if (outLastSequence)
            *outLastSequence = obs->_notifier.tracker.lastSequence();
        return (uint32_t) obs->_notifier.readChanges((slice*)outDocIDs, maxChanges, *outExternal);
    });
}


void c4dbobs_free(C4DatabaseObserver* obs) noexcept {
    if (obs) {
        WITH_LOCK(obs->_db);
        lock_guard<mutex> lock(obs->_notifier.tracker.mutex());
        delete obs;
    }
}


#pragma mark - DOCUMENT OBSERVER:


struct c4DocumentObserver : public InstanceCounted {
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
        WITH_LOCK(db);
        lock_guard<mutex> lock(db->sequenceTracker().mutex());
        return new c4DocumentObserver(db, docID, callback, context);
    });
}


void c4docobs_free(C4DocumentObserver* obs) noexcept {
    if (obs) {
        WITH_LOCK(obs->_db);
        lock_guard<mutex> lock(obs->_notifier.tracker.mutex());
        delete obs;
    }
}
