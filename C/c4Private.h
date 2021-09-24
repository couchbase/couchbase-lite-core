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
#include "c4Document.h"
#include "c4Replicator.h"

#ifdef __cplusplus
#include <atomic>
#else
#include <stdatomic.h>
#endif

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

/** Stores a C4Error in `*outError`. */
void c4error_return(C4ErrorDomain domain, int code, C4String message, C4Error *outError) C4API;

/** If set to true, LiteCore will log a warning of the form "LiteCore throwing %s error %d: %s"
    just before throwing an internal exception. This can be a good way to catch the source where
    an error occurs. */
void c4log_warnOnErrors(bool) C4API;

bool c4log_getWarnOnErrors(void) C4API;

/** Registers a handler with the C++ runtime that will log a backtrace when an uncaught C++
    exception occurs. */
void c4log_enableFatalExceptionBacktrace(void) C4API;

typedef C4_ENUM(uint32_t, C4DatabaseTag) {
    DatabaseTagAppOpened,
    DatabaseTagReplicator,
    DatabaseTagBgDB,
    DatabaseTagREST,
    DatabaseTagOther
};

C4DatabaseTag c4db_getDatabaseTag(C4Database* db) C4API;
void c4db_setDatabaseTag(C4Database* db, C4DatabaseTag dbTag) C4API;

/** Locks a recursive mutex associated with the C4Database instance.
    Blocks if it's already locked. */
void c4db_lock(C4Database *db) C4API;

/** Unlocks the mutex locked by c4db_lock. */
void c4db_unlock(C4Database *db) C4API;

/** Compiles a JSON query and returns the result set as JSON: an array with one item per result,
    and each result is an array of columns. */
C4SliceResult c4db_rawQuery(C4Database *database C4NONNULL, C4String query, C4Error *outError) C4API;

/** Subroutine of c4doc_put that reads the current revision of the document.
    Only exposed for testing; see the unit test "Document GetForPut". */
C4Document* c4doc_getForPut(C4Database *database C4NONNULL,
                            C4Slice docID,
                            C4Slice parentRevID,
                            bool deleting,
                            bool allowConflict,
                            C4Error *outError) C4API;

/** Converts C4DocumentFlags to the equivalent C4RevisionFlags. */
C4RevisionFlags c4rev_flagsFromDocFlags(C4DocumentFlags docFlags);


/** Returns the contents of the index as a Fleece-encoded array of arrays.
    (The last column of each row is the internal SQLite rowid of the document.) */
C4SliceResult c4db_getIndexRows(C4Database* database C4NONNULL,
                                C4String indexName,
                                C4Error* outError) C4API;

/** Sets the document flag kSynced. Used by the replicator to track synced documents. */
bool c4db_markSynced(C4Database *database,
                     C4String docID,
                     C4SequenceNumber sequence,
                     C4RemoteID remoteID,
                     C4Error *outError) C4API;

/** Given a list of document+revision IDs, checks whether each revision exists in the database
    or if not, what ancestors exist.

    The \ref requireBodies flag, if set, means that only ancestors whose bodies are available
    will be returned.

    The answer is written into the corresponding entry of \ref ancestors:
    * If the document doesn't exist at all, the answer will be a null slice.
    * If the document exists but not that revision, the answer will be a JSON array of existing
      ancestor revision IDs (maximum length \ref maxAncestors.)
    * If the document revision exists, the answer will be "1".
    * ... unless it's not marked as the current server revision, in which case the answer is "2" */
bool c4db_findDocAncestors(C4Database *database,
                           unsigned numDocs,
                           unsigned maxAncestors,
                           bool requireBodies,
                           C4RemoteID remoteDBID,
                           const C4String docIDs[], const C4String revIDs[],
                           C4StringResult ancestors[],
                           C4Error *outError) C4API;

#define kC4AncestorExists               C4STR("1")
#define kC4AncestorExistsButNotCurrent  C4STR("2")

/** Call this to use BuiltInWebSocket as the WebSocket implementation.
    (Only available if linked with libLiteCoreWebSocket) */
void C4RegisterBuiltInWebSocket();

#ifdef __cplusplus
}

namespace litecore { namespace websocket {
    class WebSocket;
}}


C4Replicator* c4repl_newWithWebSocket(C4Database* db,
                                      litecore::websocket::WebSocket *openSocket,
                                      C4ReplicatorParameters params,
                                      C4Error *outError) C4API;


namespace litecore { namespace constants {
    extern const C4Slice kLocalCheckpointStore;
    extern const C4Slice kPeerCheckpointStore;
    extern const C4Slice kPreviousPrivateUUIDKey;
}}

#endif
