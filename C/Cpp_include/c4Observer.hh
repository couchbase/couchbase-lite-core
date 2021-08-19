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
#include "c4Collection.hh"
#include "c4DocumentTypes.h"
#include <memory>

C4_ASSUME_NONNULL_BEGIN


// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/** A registration for callbacks whenever any document in a database changes.
    The registration lasts until this object is destructed. */
struct C4CollectionObserver : public fleece::InstanceCounted, C4Base {

    using Callback = C4Collection::CollectionObserverCallback;

    static std::unique_ptr<C4CollectionObserver> create(C4Collection*, Callback);

#ifndef C4_STRICT_COLLECTION_API
    static std::unique_ptr<C4CollectionObserver> create(C4Database*, Callback);
#endif
    
    virtual ~C4CollectionObserver() =default;

    /// Metadata of a change recorded by C4CollectionObserver. (Equivalent to C4CollectionChange.)
    struct Change {
        alloc_slice docID;              ///< Document ID
        alloc_slice revID;              ///< Revision ID
        C4SequenceNumber sequence;      ///< Sequence number, or 0 if this was a purge
        uint32_t bodySize;              ///< (Approximate) size of revision body
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


/** A registration for callbacks whenever a specific document in a collection changes.
    The registration lasts until this object is destructed. */
struct C4DocumentObserver : public fleece::InstanceCounted, C4Base {
    using Callback = C4Collection::DocumentObserverCallback;

    static std::unique_ptr<C4DocumentObserver> create(C4Collection*,
                                                      slice docID,
                                                      Callback);
    virtual ~C4DocumentObserver() =default;
protected:
    C4DocumentObserver() =default;
};

C4_ASSUME_NONNULL_END
