//
// c4ConnectedClient.h
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.h"
#include "c4ConnectedClientTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup ConnectedClient Connected Client (Remote Database)
    @{
    The Connected Client API allows you to get and put documents, and listen for changes,
    directly on a remote database server (Sync Gateway or a Couchbase Lite sync listener),
    without any local database. */

/** Creates a new connected client and starts it automatically.
    \note No need to call \ref c4client_start.
 
    @param params  Connected Client parameters.
    @param error  Error will be written here if the function fails.
    @result A new \ref C4ConnectedClient, or NULL on failure. */
C4ConnectedClient* c4client_new(const C4ConnectedClientParameters* params,
                                C4Error* error) C4API;

/** Gets the current revision of a document from the server.
    
    You can set the `unlessRevID` parameter to avoid getting a redundant copy of a
    revision you already have.
 
 @param docID  The document ID.
 @param collectionID  The name of the document's collection, or `nullslice` for default.
 @param unlessRevID  If non-null, and equal to the current server-side revision ID,
                    the server will return error {WebSocketDomain, 304}.
 @param asFleece  If true, the response's `body` field is Fleece; if false, it's JSON.
 @param callback Callback for getting document.
 @param context Client value passed to getDocument callback
 @param outError  On failure, the error info will be stored here. */
void c4client_getDoc(C4ConnectedClient*,
                     C4Slice docID,
                     C4Slice collectionID,
                     C4Slice unlessRevID,
                     bool asFleece,
                     C4ConnectedClientGetDocumentCallback callback,
                     void * C4NULLABLE context,
                     C4Error* outError) C4API;

/** Pushes a new document revision to the server.
 @param docID  The document ID.
 @param collectionID  The name of the document's collection, or `nullslice` for default.
 @param revID  The ID of the parent revision on the server,
                      or `nullslice` if this is a new document.
 @param revisionFlags  Flags of this revision.
 @param fleeceData  The document body encoded as Fleece (without shared keys!)
 @param callback Callback once the document is updated.
 @param context Client value passed to updateDocument callback
 @param outError  On failure, the error info will be stored here. */
bool c4client_putDoc(C4ConnectedClient* client,
                     C4Slice docID,
                     C4Slice collectionID,
                     C4Slice revID,
                     C4RevisionFlags revisionFlags,
                     C4Slice fleeceData,
                     C4ConnectedClientUpdateDocumentCallback callback,
                     void * C4NULLABLE context,
                     C4Error* outError) C4API;

/** Tells a connected client to start.
    \note This function is thread-safe.*/
void c4client_start(C4ConnectedClient*) C4API;

/** Tells a replicator to stop.
    \note This function is thread-safe.  */
void c4client_stop(C4ConnectedClient*) C4API;

/** Frees a connected client reference.
    Does not stop the connected client -- if the client still has other internal references,
    it will keep going. If you need the client to stop, call \ref c4client_stop first. */
void c4client_free(C4ConnectedClient*) C4API;

/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
