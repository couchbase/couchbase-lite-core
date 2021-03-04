//
// c4Observer.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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

#pragma once
#include "c4Base.hh"
#include "c4Database.hh"
#include "c4Observer.h"
#include <memory>

C4_ASSUME_NONNULL_BEGIN


/** A registration for callbacks whenever any document in a database changes.
    The registration lasts until this object is destructed. */
struct C4DatabaseObserver : public fleece::InstanceCounted, public C4Base {

    using Callback = C4Database::DatabaseObserverCallback;

    static std::unique_ptr<C4DatabaseObserver> create(C4Database*, Callback);

    virtual ~C4DatabaseObserver() =default;

    /// Metadata of a change recorded by C4DatabaseObserver.
    struct Change {
        alloc_slice docID;              ///< Document ID
        alloc_slice revID;              ///< Revision ID
        C4SequenceNumber sequence;      ///< Sequence number, or 0 if this was a purge
        C4RevisionFlags flags;          ///< Revision flags
    };

    /** Retrieves changes, in chronological order. You do not have to fetch changes immediately
        during the callback, but can wait for a convenient time, for instance scheduling a task
        on a thread/queue/event-loop.
        \note The usual way to use this method is to allocate a reasonably sized buffer, maybe
                100 changes, and keep calling getChanges passing in the entire buffer, until
                it returns 0 to indicate no more changes. */
    virtual uint32_t getChanges(Change outChanges[C4NONNULL],
                                uint32_t maxChanges,
                                bool *outExternal) =0;
};


/** A registration for callbacks whenever a specific document in a database changes.
    The registration lasts until this object is destructed. */
struct C4DocumentObserver : public fleece::InstanceCounted, public C4Base {
    using Callback = C4Database::DocumentObserverCallback;

    static std::unique_ptr<C4DocumentObserver> create(C4Database*,
                                                      slice docID,
                                                      Callback);
    virtual ~C4DocumentObserver() =default;
protected:
    C4DocumentObserver() =default;
};

C4_ASSUME_NONNULL_END
