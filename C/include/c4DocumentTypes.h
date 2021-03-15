//
// c4DocumentTypes.h
//
// Copyright (c) 2021 Couchbase, Inc All rights reserved.
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
#include "c4Base.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS


/** \defgroup Documents Documents
    @{ */


/** Flags describing a document. */
typedef C4_OPTIONS(uint32_t, C4DocumentFlags) {
    kDocDeleted         = 0x01,     ///< The document's current revision is deleted.
    kDocConflicted      = 0x02,     ///< The document is in conflict.
    kDocHasAttachments  = 0x04,     ///< The document's current revision has attachments.
    kDocExists          = 0x1000    ///< The document exists (i.e. has revisions.)
}; // Note: Superset of DocumentFlags

/** Flags that apply to a revision. */
typedef C4_OPTIONS(uint8_t, C4RevisionFlags) {
    kRevDeleted        = 0x01, ///< Is this revision a deletion/tombstone?
    kRevLeaf           = 0x02, ///< Is this revision a leaf (no children?)
    kRevNew            = 0x04, ///< Has this rev been inserted since the doc was read?
    kRevHasAttachments = 0x08, ///< Does this rev's body contain attachments?
    kRevKeepBody       = 0x10, ///< Revision's body should not be discarded when non-leaf
    kRevIsConflict     = 0x20, ///< Unresolved conflicting revision; will never be current
    kRevClosed         = 0x40, ///< Rev is the (deleted) end of a closed conflicting branch
    kRevPurged         = 0x80, ///< Revision is purged (this flag is never stored in the db)
}; // Note: Same as litecore::Rev::Flags


/** Identifies a remote database being replicated with. */
typedef uint32_t C4RemoteID;


/** Specifies how much content to retrieve when getting a document. */
typedef C4_ENUM(uint8_t, C4DocContentLevel) {
    kDocGetMetadata,            ///< Only get revID and flags
    kDocGetCurrentRev,          ///< Get current revision body but not other revisions/remotes
    kDocGetAll,                 ///< Get everything
}; // Note: Same as litecore::ContentOption


/** Describes a revision of a document. A sub-struct of C4Document. */
typedef struct C4Revision {
    C4HeapString revID;         ///< Revision ID
    C4RevisionFlags flags;      ///< Flags (deleted?, leaf?, new? hasAttachments?)
    C4SequenceNumber sequence;  ///< Sequence number in database
} C4Revision;


#ifndef LITECORE_CPP_API                        // C++ has a different declaration, in c4Document.hh
/** Describes a version-controlled document. */
struct C4Document {
    void* _internal1, *_internal2;              // placeholders for vtable-ptr and refcount in C++
#else
struct C4DocumentPublicFields {
#endif
    C4DocumentFlags flags;      ///< Document flags
    C4HeapString docID;         ///< Document ID
    C4HeapString revID;         ///< Revision ID of current revision
    C4SequenceNumber sequence;  ///< Sequence at which doc was last updated

    C4Revision selectedRev;     ///< Describes the currently-selected revision

    C4ExtraInfo extraInfo;      ///< For client use
};


/** \name Creating and Updating Documents
    @{ */


/** Optional callback to `c4doc_put` that generates the new revision body, based on an earlier
    revision body and the body of the `C4DocPutRequest`. It's intended for use when the new
    revision is specified as a delta.
    @param context  The same value given in the `C4DocPutRequest`'s `deltaCBContext` field.
    @param doc  The document; its selected revision is the one requested in the `deltaSourceRevID`.
    @param delta  The contents of the request's `body` or `allocedBody`.
    @param outError  If the callback fails, store an error here if it's non-NULL.
    @return  The body to store in the new revision, or a null slice on failure. */
typedef C4SliceResult (*C4DocDeltaApplier)(void *context,
                                           C4Document *doc,
                                           C4Slice delta,
                                           C4Error* C4NULLABLE outError);

/** Parameters for adding a revision using c4doc_put. */
typedef struct C4DocPutRequest {
    C4String body;              ///< Revision's body
    C4String docID;             ///< Document ID
    C4RevisionFlags revFlags;   ///< Revision flags (deletion, attachments, keepBody)
    bool existingRevision;      ///< Is this an already-existing rev coming from replication?
    bool allowConflict;         ///< OK to create a conflict, i.e. can parent be non-leaf?
    const C4String *history;     ///< Array of ancestor revision IDs
    size_t historyCount;        ///< Size of history[] array
    bool save;                  ///< Save the document after inserting the revision?
    uint32_t maxRevTreeDepth;   ///< Max depth of revision tree to save (or 0 for default)
    C4RemoteID remoteDBID;      ///< Identifier of remote db this rev's from (or 0 if local)

    C4SliceResult allocedBody;  ///< Set this instead of body if body is heap-allocated

    C4DocDeltaApplier C4NULLABLE deltaCB;  ///< If non-NULL, will be called to generate the actual body
    void* C4NULLABLE deltaCBContext;       ///< Passed to `deltaCB` callback
    C4String deltaSourceRevID;  ///< Source rev for delta (must be valid if deltaCB is given)
} C4DocPutRequest;


/** @} */
/** @} */


/** \defgroup Observer  Database, Document, Query Observers
 @{ */

/** \name Database Observer
 @{ */

/** Represents a change to a document in a database. */
typedef struct {
    C4HeapString docID;         ///< The document's ID
    C4HeapString revID;         ///< The latest revision ID (or null if doc was purged)
    C4SequenceNumber sequence;  ///< The latest sequence number (or 0 if doc was purged)
    C4RevisionFlags flags;
} C4DatabaseChange;

/** @} */
/** @} */


C4API_END_DECLS
C4_ASSUME_NONNULL_END
