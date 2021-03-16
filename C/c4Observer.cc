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
#include "c4Observer.h"
#include "c4Internal.hh"
#include "DatabaseImpl.hh"
#include "SequenceTracker.hh"
#include <optional>

using namespace std;


namespace litecore {

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
        Retained<DatabaseImpl> _db;
        optional<DatabaseChangeNotifier> _notifier;
        Callback _callback;
        bool _inCallback {false};
    };

}


unique_ptr<C4DatabaseObserver>
C4DatabaseObserver::create(C4Database *db, C4DatabaseObserver::Callback callback) {
    return make_unique<litecore::C4DatabaseObserverImpl>(db, UINT64_MAX, move(callback));
}


#pragma mark - DOCUMENT OBSERVER:


namespace litecore {

    struct C4DocumentObserverImpl : public C4DocumentObserver {
        C4DocumentObserverImpl(C4Database *db,
                               slice docID,
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

        Retained<DatabaseImpl> _db;
        Callback _callback;
        optional<DocChangeNotifier> _notifier;
    };

}


unique_ptr<C4DocumentObserver>
C4DocumentObserver::create(C4Database *db, slice docID, C4DocumentObserver::Callback callback) {
    return make_unique<litecore::C4DocumentObserverImpl>(db, docID, callback);
}
