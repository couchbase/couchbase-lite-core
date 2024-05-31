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
    C4HeapSlice docID;    ///< The document ID
    C4HeapSlice revID;    ///< The revision ID
    C4HeapSlice body;     ///< The document body (Fleece or JSON, as requested)
    bool        deleted;  ///< True if the document is deleted
} C4DocResponse;

typedef C4ReplicatorStatus C4ConnectedClientStatus;

/** Callback a client can register, to get status information.
    @note This will be called on arbitrary background threads, and should not block.
    @param client  The client that initiated the callback.
    @param status  The client's status.
    @param context  The `callbackContext` given in the \ref C4ConnectedClientParameters. */
typedef void (*C4ConnectedClientStatusChangedCallback)(C4ConnectedClient* client, C4ConnectedClientStatus status,
                                                       void* C4NULLABLE context);

/** Connected-client callback to provide the contents of a blob in a document you're uploading.
    This will be called after you call \ref c4client_putDoc, if the document body contains a reference
    to a blob that cannot be found in the local database.

    It's not necessary to implement this unless you plan to upload docs with custom blobs that
    aren't in the local database. If you leave the parameters' `blobProvider` field `NULL`,
    the default behavior is to return the error kC4ErrorNotFound. This will in turn cause the
    document to be rejected by the server/peer.

    @note This will be called on arbitrary background threads, and should not block for long.

    @param client  The client that initiated the callback.
    @param key  The SHA-1 digest of the blob, i.e. the decoded value of the ref's `digest` property.
    @param outError On failure, store the error here.
    @param context  The `callbackContext` given in the \ref C4ConnectedClientParameters.
    @returns  The blob data, or a NULL slice on failure. */
typedef C4SliceResult (*C4ConnectedClientBlobProviderCallback)(C4ConnectedClient* client, C4BlobKey key,
                                                               C4Error* outError, void* C4NULLABLE context);

/** Parameters describing a connected client, used when creating a \ref C4ConnectedClient. */
typedef struct C4ConnectedClientParameters {
    C4Slice                            url;                ///<URL with database to connect
    C4Slice                            optionsDictFleece;  ///< Fleece-encoded dictionary of optional parameters.
    size_t                             numCollections;     ///< Size of `collections` array
    const C4CollectionSpec* C4NULLABLE collections;        ///< Array of remote collections to access
    C4ConnectedClientStatusChangedCallback C4NULLABLE onStatusChanged;    ///< Called when status changes
    C4ConnectedClientBlobProviderCallback C4NULLABLE  blobProvider;       ///< Called while uploading a doc
    C4ReplicatorPropertyEncryptionCallback C4NULLABLE propertyEncryptor;  ///< Encryption callback
    C4ReplicatorPropertyDecryptionCallback C4NULLABLE propertyDecryptor;  ///< Decryption callback
    void* C4NULLABLE                                  callbackContext;    ///< Value passed to callbacks.
    const C4SocketFactory* C4NULLABLE                 socketFactory;      ///< Custom C4SocketFactory
} C4ConnectedClientParameters;

/** Completion callback for \ref c4client_getDoc.
    @param client  The client that initiated the callback.
    @param doc  The properties of the requested document.
    @param err Error on failure, NULL on success.
    @param context  The `callbackContext` given in the \ref C4ConnectedClientParameters. */
typedef void (*C4ConnectedClientGetDocumentCallback)(C4ConnectedClient* client, const C4DocResponse* C4NULLABLE doc,
                                                     C4Error* C4NULLABLE err, void* C4NULLABLE context);

/** Callback for updating the document result.
    @param client   The client that initiated the callback.
    @param revID  The ID of the new revision.
    @param err Error on failure, NULL on success.
    @param context  The `callbackContext` given in the \ref C4ConnectedClientParameters. */
typedef void (*C4ConnectedClientUpdateDocumentCallback)(C4ConnectedClient* client, C4HeapSlice revID,
                                                        C4Error* C4NULLABLE err, void* C4NULLABLE context);
/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
