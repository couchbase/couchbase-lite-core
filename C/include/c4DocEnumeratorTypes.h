//
// c4DocEnumeratorTypes.h
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

#include "c4DocumentTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup DocEnumerator  Document Enumeration
    @{ */


//////// DOCUMENT ENUMERATION (ALL_DOCS):


typedef C4_OPTIONS(uint16_t, C4EnumeratorFlags) {
    kC4Descending           = 0x01, ///< If true, iteration goes by descending document IDs.
    kC4Unsorted             = 0x02, ///< If true, iteration order is undefined (may be faster!)
    kC4IncludeDeleted       = 0x08, ///< If true, include deleted documents.
    kC4IncludeNonConflicted = 0x10, ///< If false, include _only_ documents in conflict.
    kC4IncludeBodies        = 0x20, /** If false, document bodies will not be preloaded, just
                               metadata (docID, revID, sequence, flags.) This is faster if you
                               don't need to access the revision tree or revision bodies. You
                               can still access all the data of the document, but it will
                               trigger loading the document body from the database. */
    kC4IncludeRevHistory    = 0x40  ///< Put entire revision history/version vector in `revID`
};


/** Options for enumerating over all documents. */
typedef struct {
    C4EnumeratorFlags flags;    ///< Option flags */
} C4EnumeratorOptions;

/** Default all-docs enumeration options. (Equal to kC4IncludeNonConflicted | kC4IncludeBodies) */
CBL_CORE_API extern const C4EnumeratorOptions kC4DefaultEnumeratorOptions;


/** Metadata about a document (actually about its current revision.) */
typedef struct C4DocumentInfo {
    C4DocumentFlags flags;      ///< Document flags
    C4HeapString docID;         ///< Document ID
    C4HeapString revID;         ///< RevID of current revision
    C4SequenceNumber sequence;  ///< Sequence at which doc was last updated
    uint64_t bodySize;          ///< Size in bytes of current revision body (as Fleece not JSON)
    uint64_t metaSize;          ///< Size in bytes of extra metadata
    int64_t expiration;         ///< Expiration time, or 0 if none
} C4DocumentInfo;


/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
