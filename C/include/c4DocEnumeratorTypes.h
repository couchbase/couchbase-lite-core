//
// c4DocEnumeratorTypes.h
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
