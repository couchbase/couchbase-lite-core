//
// c4ConnectedClientTypes.h
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
#include "c4ReplicatorTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup ConnectedClient Connected Client (Remote Database)
    @{ */

/** Result of a successful \ref c4client_getDoc call. */
typedef struct C4DocResponse {
    C4HeapSlice docID;      ///< The document ID
    C4HeapSlice revID;      ///< The revision ID
    C4HeapSlice body;       ///< The document body (Fleece or JSON, as requested)
    bool deleted;           ///< True if the document is deleted
} C4DocResponse;


/** Parameters describing a connected client, used when creating a \ref C4ConnectedClient. */
typedef struct C4ConnectedClientParameters {
    C4Slice                                           url;               ///<URL with database to connect
    C4Slice                                           optionsDictFleece; ///< Fleece-encoded dictionary of optional parameters.
    C4ReplicatorPropertyEncryptionCallback C4NULLABLE propertyEncryptor; ///< Encryption callback
    C4ReplicatorPropertyDecryptionCallback C4NULLABLE propertyDecryptor; ///< Decryption callback
    void* C4NULLABLE                                  callbackContext;   ///< Value passed to callbacks.
    const C4SocketFactory* C4NULLABLE                 socketFactory;     ///< Custom C4SocketFactory
} C4ConnectedClientParameters;


/** Callback for getting the document result.
    @param client  The client that initiated the callback.
    @param doc  Resulting document response, NULL on failure.
    @param err Error on failure, NULL on success.
    @param context  user-defined parameter given when registering the callback. */
typedef void (*C4ConnectedClientGetDocumentCallback)(C4ConnectedClient* client,
                                                     const C4DocResponse* C4NULLABLE doc,
                                                     C4Error* C4NULLABLE err,
                                                     void * C4NULLABLE context);

/** Callback for updating the document result.
 @param client   The client that initiated the callback.
 @param err Error will be written here if the get-document fails.
 @param context  user-defined parameter given when registering the callback. */
typedef void (*C4ConnectedClientUpdateDocumentCallback)(C4ConnectedClient* client,
                                                        C4HeapSlice revID,
                                                        C4Error* C4NULLABLE err,
                                                        void * C4NULLABLE context);
/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
