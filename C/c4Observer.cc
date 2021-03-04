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

#include "c4Observer.hh"
#include "c4Internal.hh"
#include "Database.hh"
#include "SequenceTracker.hh"
#include <optional>

using namespace std;


namespace c4Internal {

    struct C4DatabaseObserverImpl : public C4DatabaseObserver {
        C4DatabaseObserverImpl(C4Database *db,
                               C4SequenceNumber since,
                               Callback callback)
        :_db(asInternal(db))
        ,_callback(move(callback))
        {
            _db->sequenceTracker().use<>([&](SequenceTracker &st) {
                _notifier.emplace(st,
                                  [this](DatabaseChangeNotifier&) {_callback(this);},
                                  since);
            });
        }


        ~C4DatabaseObserverImpl() {
            _db->sequenceTracker().use([&](SequenceTracker &st) {
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
                          "C4DatabaseObserver::Change doesn't match SequenceTracker::Change");
            return _db->sequenceTracker().use<uint32_t>([&](SequenceTracker &st) {
                return (uint32_t) _notifier->readChanges((SequenceTracker::Change*)outChanges,
                                                          maxChanges,
                                                          *outExternal);
            });
        }

    private:
        Retained<Database> _db;
        optional<DatabaseChangeNotifier> _notifier;
        Callback _callback;
        bool _inCallback {false};
    };

    static C4DatabaseObserverImpl* asInternal(C4DatabaseObserver *o) {
        return (C4DatabaseObserverImpl*)o;
    }

}


unique_ptr<C4DatabaseObserver>
C4DatabaseObserver::create(C4Database *db, C4DatabaseObserver::Callback callback) {
    return make_unique<C4DatabaseObserverImpl>(db, UINT64_MAX, move(callback));
}


C4DatabaseObserver* c4dbobs_create(C4Database *db,
                                   C4DatabaseObserverCallback callback,
                                   void *context) noexcept
{
    return C4DatabaseObserver::create(db, [=](C4DatabaseObserver *obs) {
        callback(obs, context);
    }).release();
}


uint32_t c4dbobs_getChanges(C4DatabaseObserver *obs,
                            C4DatabaseChange outChanges[],
                            uint32_t maxChanges,
                            bool *outExternal) noexcept
{
    static_assert(sizeof(C4DatabaseChange) == sizeof(C4DatabaseObserver::Change),
                  "C4DatabaseChange doesn't match C4DatabaseObserver::Change");
    return tryCatch<uint32_t>(nullptr, [&]{
        memset(outChanges, 0, maxChanges * sizeof(C4DatabaseChange));
        return asInternal(obs)->getChanges((C4DatabaseObserver::Change*)outChanges,
                                           maxChanges, outExternal);
        // This is slightly sketchy because C4DatabaseObserver::Change contains alloc_slices, whereas
        // C4DatabaseChange contains slices. The result is that the docID and revID memory will be
        // temporarily leaked, since the alloc_slice destructors won't be called.
        // For this purpose we have c4dbobs_releaseChanges(), which does the same sleight of hand
        // on the array but explicitly destructs each Change object, ensuring its alloc_slices are
        // destructed and the backing store's ref-count goes back to what it was originally.
    });
}


void c4dbobs_releaseChanges(C4DatabaseChange changes[], uint32_t numChanges) noexcept {
    for (uint32_t i = 0; i < numChanges; ++i) {
        auto &change = (C4DatabaseObserver::Change&)changes[i];
        change.~Change();
    }
}


void c4dbobs_free(C4DatabaseObserver* obs) noexcept {
    delete obs;
}


#pragma mark - DOCUMENT OBSERVER:


namespace c4Internal {

    struct C4DocumentObserverImpl : public C4DocumentObserver {
        C4DocumentObserverImpl(C4Database *db,
                            C4Slice docID,
                            Callback callback)
        :_db(asInternal(db))
        ,_callback(callback)
        {
            _db->sequenceTracker().use<>([&](SequenceTracker &st) {
                _notifier.emplace(st,
                                  docID,
                                  [this](DocChangeNotifier&, slice docID, sequence_t sequence) {
                                        _callback(this, docID, sequence);
                                  });
            });
        }

        ~C4DocumentObserverImpl() {
            _db->sequenceTracker().use([&](SequenceTracker &st) {
                // Clearing (destructing) the notifier stops me from getting calls.
                // I do this explicitly, synchronized with the SequenceTracker.
                _notifier = nullopt;
            });
        }

        Retained<Database> _db;
        Callback _callback;
        optional<DocChangeNotifier> _notifier;
    };

}


unique_ptr<C4DocumentObserver>
C4DocumentObserver::create(C4Database *db, slice docID, C4DocumentObserver::Callback callback) {
    return make_unique<C4DocumentObserverImpl>(db, docID, callback);
}


C4DocumentObserver* c4docobs_create(C4Database *db,
                                    C4Slice docID,
                                    C4DocumentObserverCallback callback,
                                    void *context) noexcept
{
    return tryCatch<unique_ptr<C4DocumentObserver>>(nullptr, [&]{
        auto fn = [=](C4DocumentObserver *obs, fleece::slice docID, C4SequenceNumber seq) {
            callback(obs, docID, seq, context);
        };
        return C4DocumentObserverImpl::create(db, docID, fn);
    }).release();
}


void c4docobs_free(C4DocumentObserver* obs) noexcept {
    delete obs;
}
