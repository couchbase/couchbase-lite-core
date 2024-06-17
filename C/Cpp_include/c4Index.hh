//
// c4Index.hh
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "c4IndexTypes.h"
#include "fleece/InstanceCounted.hh"

C4_ASSUME_NONNULL_BEGIN

// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/** Represents an index. Acts as a factory for C4IndexUpdater objects. */
struct C4Index
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4Index>
    , C4Base {
    C4Collection* getCollection() const { return _collection; }

    slice getName() const { return _name; }

#ifdef COUCHBASE_ENTERPRISE
    /// Finds new or updated documents for which vectors need to be recomputed by the application.
    /// If there are none, returns NULL.
    /// @param limit  The maximum number of documents/vectors to return. If this is less than
    ///               the total number, the rest will be returned on the next call to `beginUpdate`.
    /// @warning  Do not call `beginUpdate` again until you're done with the returned updater;
    ///           it's not valid to have more than one update in progress at a time.
    Retained<struct C4IndexUpdater> beginUpdate(size_t limit);
#endif

  protected:
    friend class litecore::CollectionImpl;
    static Retained<C4Index> getIndex(C4Collection*, slice name);

    Retained<C4Collection> _collection;
    std::string            _name;
};

#ifdef COUCHBASE_ENTERPRISE

/** Describes a set of index values that need to be computed by the application,
    to update a lazy index after its Collection has changed.
    You should:

    1. Call `valueAt` for each of the `count` items to get the Fleece value, and:
      1.1. Compute a vector from this value
      1.2. Call `setVectorAt` with the resulting vector, or with nullptr if none.
    2. Finally, open a transaction and call `finish` to apply the updates to the index.

    If you need to abandon an update, simply release the updater without calling `finish`. */
struct C4IndexUpdater final
    : public fleece::RefCounted
    , public fleece::InstanceCountedIn<C4IndexUpdater>
    , C4Base {
    /// The number of vectors to compute.
    size_t count() const;

    /// Returns the i'th value to compute a vector from.
    /// This is the value of the expression in the index spec.
    FLValue valueAt(size_t i) const;

    /// Sets the vector for the i'th value. A NULL pointer means there is no vector and any
    /// existing vector should be removed from the index.
    void setVectorAt(size_t i, const float* C4NULLABLE vector, size_t dimension);

    /// Tells the updater that the i'th vector can't be computed at this time, e.g. because of
    /// a transient network error. The associated document will be returned again in the next
    /// call to `C4Index::beginUpdate()`.
    void skipVectorAt(size_t i);

    /// Updates the index with the computed vectors, removes any index rows for which no vector
    /// was given, and updates the index's latest sequence.
    /// @returns  True if the index is now completely up-to-date; false if there are more vectors
    ///           to index (including ones changed since the call to `C4Index::beginUpdate`.)
    bool finish();

  private:
    friend struct C4IndexImpl;
    C4IndexUpdater(Retained<litecore::LazyIndexUpdate>, C4Collection*);
    ~C4IndexUpdater();

    // Invariants: _update != nullptr || (finish() has been called)
    bool hasFinished() const { return !_update; }

    Retained<litecore::LazyIndexUpdate> _update;
    Retained<C4Collection>              _collection;
};

#endif

C4_ASSUME_NONNULL_END
