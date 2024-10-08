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
#include "CollectionImpl.hh"
#include "SequenceTracker.hh"
#include <optional>
#include <utility>

using namespace std;

namespace litecore {

    class C4CollectionObserverImpl : public C4CollectionObserver {
      public:
        C4CollectionObserverImpl(C4Collection* collection, C4SequenceNumber since, Callback callback)
            : _retainDatabase(collection->getDatabase())
            , _collection(asInternal(collection))
            , _callback(std::move(callback)) {
            _collection->sequenceTracker().useLocked<>([&](SequenceTracker& st) {
                _notifier.emplace(
                        &st, [this](CollectionChangeNotifier&) { _callback(this); }, since);
            });
        }

        ~C4CollectionObserverImpl() override {
            if ( !_collection->isValid() ) {
                // HACK: If the collection is not valid anymore, the notifier tracker is probably
                // also bad, so null it out so the destructor doesn't try to use it
                _notifier->tracker = nullptr;
                return;
            }

            _collection->sequenceTracker().useLocked([&](SequenceTracker& st) {
                // Clearing (destructing) the notifier stops me from getting calls.
                // I do this explicitly, synchronized with the SequenceTracker.
                _notifier = nullopt;
            });
        }

        C4CollectionObservation getChanges(Change outChanges[], uint32_t maxChanges) override {
            static_assert(sizeof(Change) == sizeof(SequenceTracker::Change),
                          "C4CollectionObserver::Change doesn't match SequenceTracker::Change");
            return _collection->sequenceTracker().useLocked<C4CollectionObservation>([&](SequenceTracker& st) {
                bool outExternal;
                auto retVal =
                        (uint32_t)_notifier->readChanges((SequenceTracker::Change*)outChanges, maxChanges, outExternal);

                return C4CollectionObservation{retVal, outExternal, _collection};
            });
        }

      private:
        Retained<C4Database>               _retainDatabase;
        Retained<CollectionImpl>           _collection;
        optional<CollectionChangeNotifier> _notifier;
        Callback                           _callback;
    };

}  // namespace litecore

unique_ptr<C4CollectionObserver> C4CollectionObserver::create(C4Collection*                  coll,
                                                              C4CollectionObserver::Callback callback) {
    return make_unique<litecore::C4CollectionObserverImpl>(coll, C4SequenceNumber::Max, std::move(callback));
}

#pragma mark - DOCUMENT OBSERVER:

namespace litecore {

    class C4DocumentObserverImpl : public C4DocumentObserver {
      public:
        C4DocumentObserverImpl(C4Collection* collection, slice docID, Callback callback)
            : _retainedDatabase(collection->getDatabase())
            , _collection(asInternal(collection))
            , _callback(std::move(callback)) {
            _collection->sequenceTracker().useLocked<>([&](SequenceTracker& st) {
                _notifier.emplace(&st, docID, [this](DocChangeNotifier&, slice docID, sequence_t sequence) {
                    _callback(this, _collection, docID, sequence);
                });
            });
        }

        ~C4DocumentObserverImpl() override {
            if ( !_collection->isValid() ) {
                // HACK: If the collection is not valid anymore, the notifier tracker is probably
                // also bad, so null it out so the destructor doesn't try to use it
                _notifier->tracker = nullptr;
                return;
            }

            _collection->sequenceTracker().useLocked([&](SequenceTracker& st) {
                // Clearing (destructing) the notifier stops me from getting calls.
                // I do this explicitly, synchronized with the SequenceTracker.
                _notifier = nullopt;
            });
        }

      private:
        Retained<C4Database>        _retainedDatabase;
        Retained<CollectionImpl>    _collection;
        Callback                    _callback;
        optional<DocChangeNotifier> _notifier;
    };

}  // namespace litecore

unique_ptr<C4DocumentObserver> C4DocumentObserver::create(C4Collection* db, slice docID,
                                                          const C4DocumentObserver::Callback& callback) {
    return make_unique<litecore::C4DocumentObserverImpl>(db, docID, callback);
}
