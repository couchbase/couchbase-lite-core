//
// c4Private.h
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

#pragma once
#include "c4DocumentTypes.h"
#include <stdarg.h>

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

// This file contains LiteCore C APIs that are intended only for use by the replicator and
// listener implementations, and the `cblite` tool, not by Couchbase Lite implementations or others.
// This stuff can change without warning.

C4_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
extern "C" {

    /** If > 0, the currently running test is expected to throw an exception, so debuggers should
        ignore the exception. */
    CBL_CORE_API extern std::atomic_int gC4ExpectExceptions;

#else
    CBL_CORE_API extern atomic_int gC4ExpectExceptions;
#endif


/** Allocates a C4SliceResult by copying the memory of a slice. */
C4SliceResult c4slice_createResult(C4Slice slice);

/** Locks a recursive mutex associated with the C4Database instance.
    Blocks if it's already locked. */
void c4db_lock(C4Database *db) C4API;

/** Unlocks the mutex locked by c4db_lock. */
void c4db_unlock(C4Database *db) C4API;

/** Compiles a JSON query and returns the result set as JSON: an array with one item per result,
    and each result is an array of columns. */
C4SliceResult c4db_rawQuery(C4Database *database, C4String query, C4Error* C4NULLABLE outError) C4API;

/** Converts C4DocumentFlags to the equivalent C4RevisionFlags. */
C4RevisionFlags c4rev_flagsFromDocFlags(C4DocumentFlags docFlags) C4API;


/** Returns the contents of the index as a Fleece-encoded array of arrays.
    (The last column of each row is the internal SQLite rowid of the document.) */
C4SliceResult c4db_getIndexRows(C4Database* database,
                                C4String indexName,
                                C4Error* C4NULLABLE error) C4API;

C4StringResult c4db_getPeerID(C4Database* database) C4API;

/** Sets the document flag kSynced. Used by the replicator to track synced documents. */
bool c4db_markSynced(C4Database *database,
                     C4String docID,
                     C4String revID,
                     C4SequenceNumber sequence,
                     C4RemoteID remoteID,
                     C4Error* C4NULLABLE outError) C4API;


/** Flags produced by \ref c4db_findDocAncestors, the result of comparing a local document's
    revision(s) against the requested revID. */
typedef C4_OPTIONS(uint8_t, C4FindDocAncestorsResultFlags) {
    kRevsSame           = 0,    // Current revision is equal
    kRevsLocalIsOlder   = 1,    // Current revision is older
    kRevsLocalIsNewer   = 2,    // Current revision is newer
    kRevsConflict       = 3,    // Current revision conflicts (== LocalIsOlder | LocalIsNewer)
    kRevsAtThisRemote   = 4,    // The given C4RemoteID has this revID
    kRevsHaveLocal      = 8,    // Local doc has this revID with its body
};

/** Given a list of document+revision IDs, checks whether each revision exists in the database
    or if not, what ancestors exist.

    The \ref requireBodies flag, if set, means that only ancestors whose bodies are available
    will be considered.

    The answer is written into the corresponding entry of \ref ancestors:
    * If the document doesn't exist at all, the answer will be a null slice.
    * If the document exists but not that revision, the answer will be a JSON array of existing
      ancestor revision IDs (maximum length \ref maxAncestors.)
    * If the document revision exists, the answer will be `kC4AncestorExists` ("1").
    * ... unless it's not marked as the current server revision for `remoteDBID`, in which case the answer is
     `kC4AncestorExistsButNotCurrent` "2" */
bool c4db_findDocAncestors(C4Database *database,
                           unsigned numDocs,
                           unsigned maxAncestors,
                           bool requireBodies,
                           C4RemoteID remoteDBID,
                           const C4String docIDs[C4NONNULL],
                           const C4String revIDs[C4NONNULL],
                           C4StringResult ancestors[C4NONNULL],
                           C4Error* C4NULLABLE outError) C4API;

/** Call this to use BuiltInWebSocket as the WebSocket implementation.
    (Only available if linked with libLiteCoreWebSocket) */
void C4RegisterBuiltInWebSocket();


#ifdef __cplusplus
}

// C++-only stuff:

namespace litecore::websocket {
    class WebSocket;
}


C4Replicator* c4repl_newWithWebSocket(C4Database* db,
                                      litecore::websocket::WebSocket *openSocket,
                                      struct C4ReplicatorParameters params,
                                      C4Error* C4NULLABLE outError) C4API;


namespace litecore::constants {
    extern const C4Slice kLocalCheckpointStore;
    extern const C4Slice kPeerCheckpointStore;
    extern const C4Slice kPreviousPrivateUUIDKey;
}

#endif

C4_ASSUME_NONNULL_END
