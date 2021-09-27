//
// c4DocEnumerator.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "c4DocEnumeratorTypes.h"
#include <memory>

C4_ASSUME_NONNULL_BEGIN


// ************************************************************************
// This header is part of the LiteCore C++ API.
// If you use this API, you must _statically_ link LiteCore;
// the dynamic library only exports the C API.
// ************************************************************************


/** Iterates the documents in the collection, by docID or by sequence or unsorted. */
struct C4DocEnumerator : public fleece::InstanceCounted, C4Base {
    /// Creates an enumerator on a collection, ordered by docID (unless the `kC4Unsorted` flag
    /// is set.)
    /// You must first call \ref next to step to the first document.
    explicit C4DocEnumerator(C4Collection *collection,
                             const C4EnumeratorOptions &options = kC4DefaultEnumeratorOptions);

    /// Creates an enumerator on a collection, ordered by sequence.
    /// You must first call \ref next to step to the first document.
    explicit C4DocEnumerator(C4Collection *collection,
                             C4SequenceNumber since,
                             const C4EnumeratorOptions &options = kC4DefaultEnumeratorOptions);

#ifndef C4_STRICT_COLLECTION_API
    explicit C4DocEnumerator(C4Database*, const C4EnumeratorOptions& = kC4DefaultEnumeratorOptions);
    explicit C4DocEnumerator(C4Database*, C4SequenceNumber, const C4EnumeratorOptions& = kC4DefaultEnumeratorOptions);
#endif

    ~C4DocEnumerator();

    /// Stores the current document's metadata into a struct,
    /// or returns false if the enumerator is finished.
    bool getDocumentInfo(C4DocumentInfo&) const noexcept;

    /// Returns the current document's metadata, or throws an exception if finished.
    C4DocumentInfo documentInfo() const;

    /// Returns the current document.
    /// \note If you use this, it's usually a good idea to set the `kC4IncludeBodies` option flag,
    /// so that the document bodies will be preloaded, saving a second database hit.
    Retained<C4Document> getDocument() const;

    /// Steps to the next document. Returns false when it reaches the end.
    bool next();

    /// Tears down the internal state without destructing this object. This is useful to free up
    /// resources if the destructor might not be called immediately (i.e. if it's waiting for a
    /// GC finalizer to run.)
    void close() noexcept;

private:
    C4DocEnumerator(const C4DocEnumerator&) = delete;
    
    class Impl;
    std::unique_ptr<Impl> _impl;
};

C4_ASSUME_NONNULL_END
