//
// c4Observer.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Observer.hh"
#include "c4Database.hh"
#include "c4Internal.hh"
#include "CollectionImpl.hh"
#include "SequenceTracker.hh"
#include <optional>

using namespace std;


namespace litecore {

    class C4CollectionObserverImpl : public C4CollectionObserver {
    public:
        C4CollectionObserverImpl(C4Collection *collection,
                                 C4SequenceNumber since,
                                 Callback callback)
        :_retainDatabase(collection->getDatabase())
        ,_collection(asInternal(collection))
        ,_callback(move(callback))
        {
            _collection->sequenceTracker().useLocked<>([&](SequenceTracker &st) {
                _notifier.emplace(st,
                                  [this](CollectionChangeNotifier&) {_callback(this);},
                                  since);
            });
        }


        ~C4CollectionObserverImpl() {
            _collection->sequenceTracker().useLocked([&](SequenceTracker &st) {
                // Clearing (destructing) the notifier stops me from getting calls.
                // I do this explicitly, synchronized with the SequenceTracker.
                _notifier = nullopt;
            });
        }


        uint32_t getChanges(Change outChanges[],
                            uint32_t maxChanges,
                            bool *outExternal) override
        {
            static_assert(sizeof(Change) == sizeof(SequenceTracker::Change),
                          "C4CollectionObserver::Change doesn't match SequenceTracker::Change");
            return _collection->sequenceTracker().useLocked<uint32_t>([&](SequenceTracker &st) {
                return (uint32_t) _notifier->readChanges((SequenceTracker::Change*)outChanges,
                                                          maxChanges,
                                                          *outExternal);
            });
        }

    private:
        Retained<C4Database> _retainDatabase;
        CollectionImpl* _collection;
        optional<CollectionChangeNotifier> _notifier;
        Callback _callback;
        bool _inCallback {false};
    };

}


unique_ptr<C4CollectionObserver>
C4CollectionObserver::create(C4Collection *coll, C4CollectionObserver::Callback callback) {
    return make_unique<litecore::C4CollectionObserverImpl>(coll, C4SequenceNumber::Max, move(callback));
}


#ifndef C4_STRICT_COLLECTION_API
unique_ptr<C4CollectionObserver>
C4CollectionObserver::create(C4Database *db, Callback callback) {
    return create(db->getDefaultCollection(), callback);
}
#endif


#pragma mark - DOCUMENT OBSERVER:


namespace litecore {

    class C4DocumentObserverImpl : public C4DocumentObserver {
    public:
        C4DocumentObserverImpl(C4Collection *collection,
                               slice docID,
                               Callback callback)
        :_collection(asInternal(collection))
        ,_callback(callback)
        {
            _collection->sequenceTracker().useLocked<>([&](SequenceTracker &st) {
                _notifier.emplace(st,
                                  docID,
                                  [this](DocChangeNotifier&, slice docID, sequence_t sequence) {
                                        _callback(this, docID, sequence);
                                  });
            });
        }

        ~C4DocumentObserverImpl() {
            _collection->sequenceTracker().useLocked([&](SequenceTracker &st) {
                // Clearing (destructing) the notifier stops me from getting calls.
                // I do this explicitly, synchronized with the SequenceTracker.
                _notifier = nullopt;
            });
        }
        
    private:
        CollectionImpl* _collection;
        Callback _callback;
        optional<DocChangeNotifier> _notifier;
    };

}


unique_ptr<C4DocumentObserver>
C4DocumentObserver::create(C4Collection *db, slice docID, C4DocumentObserver::Callback callback) {
    return make_unique<litecore::C4DocumentObserverImpl>(db, docID, callback);
}
