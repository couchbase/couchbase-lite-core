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

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** Result of a successful `c4client_getDoc` call. */
typedef struct C4DocResponse {
    C4HeapSlice docID;
    C4HeapSlice revID;
    C4HeapSlice body;
    bool deleted;
} C4DocResponse;

/** Parameters describing a connected client, used when creating a C4ConnectedClient. */
typedef struct C4ConnectedClientParameters {
    C4Slice                             url;               ///<URL with database to connect
    C4Slice                             options;           ///< Fleece-encoded dictionary of optional parameters.
    const C4SocketFactory* C4NULLABLE   socketFactory;     ///< Custom C4SocketFactory
    void* C4NULLABLE                    callbackContext;   ///< Value to be passed to the callbacks.
} C4ConnectedClientParameters;

/** Callback for getting the document result.
 
 @param client   The client that initiated the callback.
 @param doc  Resuting document response, NULL on failure.
 @param err Error will be written here if the get-document fails.
 @param context  user-defined parameter given when registering the callback. */
typedef void (*C4ConnectedClientDocumentResultCallback)(C4ConnectedClient* client,
                                                        const C4DocResponse* C4NULLABLE doc,
                                                        C4Error* C4NULLABLE err,
                                                        void * C4NULLABLE context);

typedef C4ConnectedClientDocumentResultCallback C4ConnectedClientDocumentResultCallback;

/** Creates a new connected client and starts it automatically.
    \note No need to call the c4client_start().
 
    @param params  Connected Client parameters (see above.)
    @param error  Error will be written here if the function fails.
    @result A new C4ConnectedClient, or NULL on failure. */
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
 @param error  On failure, the error info will be stored here. */
void c4client_getDoc(C4ConnectedClient*,
                     C4Slice docID,
                     C4Slice collectionID,
                     C4Slice unlessRevID,
                     bool asFleece,
                     C4ConnectedClientDocumentResultCallback callback,
                     void * C4NULLABLE context,
                     C4Error* error) C4API;

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

C4API_END_DECLS
C4_ASSUME_NONNULL_END
