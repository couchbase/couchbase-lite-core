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


struct c4DatabaseObserver : public DatabaseChangeNotifier, InstanceCounted {
    c4DatabaseObserver(C4Database *db, C4SequenceNumber since,
                       C4DatabaseObserverCallback callback, void *context)
    :DatabaseChangeNotifier(db->sequenceTracker(),
                            bind(&c4DatabaseObserver::dispatchCallback, this, _1),
                            since),
     _callback(callback),
     _context(context),
     _db(db)
    { }


    void dispatchCallback(DatabaseChangeNotifier&) {
        _callback(this, _context);
    }

    C4DatabaseObserverCallback _callback;
    void *_context;
    Retained<Database> _db;
};


C4DatabaseObserver* c4dbobs_create(C4Database *db,
                                   C4DatabaseObserverCallback callback,
                                   void *context) noexcept
{
    return tryCatch<C4DatabaseObserver*>(nullptr, [&]{
        WITH_LOCK(db);
        return new c4DatabaseObserver(db, UINT64_MAX, callback, context);
    });
}


uint32_t c4dbobs_getChanges(C4DatabaseObserver *obs,
                            C4Slice outDocIDs[],
                            uint32_t maxChanges,
                            C4SequenceNumber* outLastSequence) noexcept
{
    return tryCatch<uint32_t>(nullptr, [&]{
        WITH_LOCK(obs->_db);
        return (uint32_t) obs->readChanges((slice*)outDocIDs, maxChanges);
    });
}


void c4dbobs_free(C4DatabaseObserver* obs) noexcept {
    if (obs) {
        WITH_LOCK(obs->_db);
        delete obs;
    }
}


#pragma mark - DOCUMENT OBSERVER:


struct c4DocumentObserver : public DocChangeNotifier, InstanceCounted {
    c4DocumentObserver(C4Database *db, C4Slice docID,
                       C4DocumentObserverCallback callback, void *context)
    :DocChangeNotifier(db->sequenceTracker(),
                       docID,
                       bind(&c4DocumentObserver::dispatchCallback, this, _1, _2, _3)),
     _callback(callback),
     _context(context),
     _db(db)
    { }


    void dispatchCallback(DocChangeNotifier&, slice docID, sequence_t sequence) {
        _callback(this, docID, sequence, _context);
    }

    C4DocumentObserverCallback _callback;
    void *_context;
    Retained<Database> _db;
};


C4DocumentObserver* c4docobs_create(C4Database *db,
                                    C4Slice docID,
                                    C4DocumentObserverCallback callback,
                                    void *context) noexcept
{
    return tryCatch<C4DocumentObserver*>(nullptr, [&]{
        WITH_LOCK(db);
        return new c4DocumentObserver(db, docID, callback, context);
    });
}


void c4docobs_free(C4DocumentObserver* obs) noexcept {
    if (obs) {
        WITH_LOCK(obs->_db);
        delete obs;
    }
}
