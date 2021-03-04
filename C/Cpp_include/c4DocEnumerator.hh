//
// c4DocEnumerator.hh
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
#include "c4DocEnumerator.h"
#include <memory>

C4_ASSUME_NONNULL_BEGIN


/** Iterates the documents in the database, by docID or by sequence or unsorted. */
struct C4DocEnumerator : public C4Base {
    /// Creates an enumerator on a database, ordered by docID (unless the `kC4Unsorted` flag
    /// is set.)
    /// You must first call \ref next to step to the first document.
    explicit C4DocEnumerator(C4Database *database,
                             const C4EnumeratorOptions &options = kC4DefaultEnumeratorOptions);

    /// Creates an enumerator on a database, ordered by sequence.
    /// You must first call \ref next to step to the first document.
    explicit C4DocEnumerator(C4Database *database,
                             C4SequenceNumber since,
                             const C4EnumeratorOptions &options = kC4DefaultEnumeratorOptions);

    ~C4DocEnumerator();

    /// Stores the current document's metadata into a struct,
    /// or returns false if the enumerator is finished.
    bool getDocumentInfo(C4DocumentInfo&) const noexcept;

    /// Returns the current document's metadata, or throws an exception if finished.
    C4DocumentInfo documentInfo() const;

    /// Returns the current document.
    /// \note If you use this, it's usually a good idea to set the `kC4IncludeBodies` option flag,
    /// so that the document bodies will be preloaded, saving a second database hit.
    Retained<C4Document> document() const;

    /// Steps to the next document. Returns false when it reaches the end.
    bool next();

    /// Tears down the internal state without destructing this object. This is useful to free up
    /// resources if the destructor might not be called immediately (i.e. if it's waiting for a
    /// GC finalizer to run.)
    void close() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

C4_ASSUME_NONNULL_END
