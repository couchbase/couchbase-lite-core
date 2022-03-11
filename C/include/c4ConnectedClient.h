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
typedef struct {
    C4Slice docID;
    C4Slice revID;
    C4Slice body;
    bool deleted;
} C4DocResponse;

/** Callback for getting the document result.
 
 @param client   The client that initiated the callback.
 @param doc  Resuting document response.
 @param context  user-defined parameter given when registering the callback. */
typedef void (*C4ConnectedClientDocumentResultCallback)(C4ConnectedClient* client,
                                                C4DocResponse doc,
                                                void * C4NULLABLE context);
/** Creates a new connected client
    @param socket  The web socket through which it is connected
    @param options  Fleece-encoded dictionary of optional parameters.
    @param error  Error will be written here if the function fails.
    @result A new C4ConnectedClient, or NULL on failure. */
C4ConnectedClient* c4client_new(C4Socket* socket,
                                C4Slice options,
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
                     C4ConnectedClientDocumentResultCallback C4NULLABLE callback,
                     void * C4NULLABLE context,
                     C4Error* error) C4API;

C4API_END_DECLS
C4_ASSUME_NONNULL_END
