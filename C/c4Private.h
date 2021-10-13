//
// c4Private.h
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
C4API_BEGIN_DECLS

#ifdef __cplusplus
    /** If > 0, the currently running test is expected to throw an exception, so debuggers should
        ignore the exception. */
    CBL_CORE_API extern std::atomic_int gC4ExpectExceptions;
#else
    CBL_CORE_API extern atomic_int gC4ExpectExceptions;
#endif


/** Compiles a JSON query and returns the result set as JSON: an array with one item per result,
    and each result is an array of columns. */
C4SliceResult c4db_rawQuery(C4Database *database, C4String query, C4Error* C4NULLABLE outError) C4API;

///** Converts C4DocumentFlags to the equivalent C4RevisionFlags. */
C4RevisionFlags c4rev_flagsFromDocFlags(C4DocumentFlags docFlags) C4API;


/** Returns the contents of the index as a Fleece-encoded array of arrays.
    (The last column of each row is the internal SQLite rowid of the document.) */
C4SliceResult c4db_getIndexRows(C4Database* database,
                                C4String indexName,
                                C4Error* C4NULLABLE error) C4API;

C4StringResult c4db_getPeerID(C4Database* database) C4API;

typedef C4_ENUM(uint32_t, C4DatabaseTag) {
    DatabaseTag_AppOpened,
    DatabaseTag_DBAccess,
    DatabaseTag_C4RemoteReplicator,
    DatabaseTag_C4IncomingReplicator,
    DatabaseTag_C4LocalReplicator1,
    DatabaseTag_C4LocalReplicator2,
    DatabaseTag_BackgroundDB,
    DatabaseTag_RESTListener
};

C4DatabaseTag _c4db_getDatabaseTag(C4Database* db) C4API;
void _c4db_setDatabaseTag(C4Database* db, C4DatabaseTag dbTag) C4API;

/** Sets the document flag kSynced. Used by the replicator to track synced documents. */
bool c4db_markSynced(C4Database *database,
                     C4String docID,
                     C4String revID,
                     C4SequenceNumber sequence,
                     C4RemoteID remoteID,
                     C4Error* C4NULLABLE outError) C4API;

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


C4API_END_DECLS
C4_ASSUME_NONNULL_END
