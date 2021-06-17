//
// c4BlobStore.h
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
#include "c4BlobStoreTypes.h"
#include "c4DatabaseTypes.h"
#include <stdio.h>

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup Blobs Blobs
        @{ */

    /** \name Blob Keys
        @{ */


    /** Decodes a string of the form "sha1-"+base64 into a raw key. */
    bool c4blob_keyFromString(C4String str, C4BlobKey*) C4API;

    /** Encodes a blob key to a string of the form "sha1-"+base64. */
    C4StringResult c4blob_keyToString(C4BlobKey) C4API;

    /** @} */


    //////// BLOB STORE API:

    /** \name Blob Store API
        @{ */

    /** Returns the BlobStore associated with a bundled database.
        Fails if the database is not bundled.
        DO NOT call c4blob_freeStore on this! The C4Database will free it when it closes. */
    C4BlobStore* c4db_getBlobStore(C4Database *db, C4Error* C4NULLABLE outError) C4API;

    /** Opens a BlobStore in a directory. If the flags allow creating, the directory will be
        created if necessary.
        Call c4blob_freeStore() when finished using the BlobStore.
        \warning This should only be used for unit testing. Naked BlobStores are not supported.
        @param dirPath  The filesystem path of the directory holding the attachments.
        @param flags  Specifies options like create, read-only
        @param encryptionKey  Optional encryption algorithm & key
        @param outError  Error is returned here
        @return  The BlobStore reference, or NULL on error */
    C4BlobStore* c4blob_openStore(C4String dirPath,
                                  C4DatabaseFlags flags,
                                  const C4EncryptionKey* C4NULLABLE encryptionKey,
                                  C4Error* C4NULLABLE outError) C4API;

    /** Closes/frees a BlobStore. (A NULL parameter is allowed.)
        \warning This should only be used for unit testing. Never free a BlobStore belonging to a
                 C4Database. */
    void c4blob_freeStore(C4BlobStore* C4NULLABLE) C4API;

    /** Deletes the BlobStore's blobs and directory, and (if successful) frees the object.
        \warning This should only be used for unit testing. Never delete a BlobStore belonging to a
                 C4Database. */
    bool c4blob_deleteStore(C4BlobStore*, C4Error* C4NULLABLE) C4API;

    /** @} */


    /** \name Blob API
        @{ */

    /* NOTE: Every function in this section is thread-safe, as long as the C4BlobStore
       reference remains valid while the function executes, i.e. there are no concurrent calls
       to c4blob_freeStore, c4blob_deleteStore or c4db_close. */

    /** Gets the content size of a blob given its key. Returns -1 if it doesn't exist.
        WARNING: If the blob is encrypted, the return value is a conservative estimate that may
        be up to 16 bytes larger than the actual size. */
    int64_t c4blob_getSize(C4BlobStore*, C4BlobKey) C4API;

    /** Reads the entire contents of a blob into memory. Caller is responsible for freeing it. */
    C4SliceResult c4blob_getContents(C4BlobStore*, C4BlobKey, C4Error* C4NULLABLE) C4API;

    /** Returns the path of the file that stores the blob, if possible. This call may fail with
        error kC4ErrorWrongFormat if the blob is encrypted (in which case the file would be
        unreadable by the caller) or with kC4ErrorUnsupported if for some implementation reason
        the blob isn't stored as a standalone file.
        Thus, the caller MUST use this function only as an optimization, and fall back to reading
        the contents via the API if it fails.
        Also, it goes without saying that the caller MUST not modify the file! */
    C4StringResult c4blob_getFilePath(C4BlobStore*, C4BlobKey, C4Error* C4NULLABLE) C4API;

    /** Derives the key of the given data, without storing it. */
    C4BlobKey c4blob_computeKey(C4Slice contents);

    /** Stores a blob. The associated key will be written to `outKey`, if non-NULL.
        If `expectedKey` is not NULL, then the operation will fail unless the contents actually
        have that key. */
    bool c4blob_create(C4BlobStore *store,
                       C4Slice contents,
                       const C4BlobKey *C4NULLABLE expectedKey,
                       C4BlobKey* C4NULLABLE outKey,
                       C4Error* C4NULLABLE error) C4API;

    /** Deletes a blob from the store given its key. */
    bool c4blob_delete(C4BlobStore*, C4BlobKey, C4Error* C4NULLABLE) C4API;

    /** @} */


    //////// STREAMING API:

    /** \name Streamed Reads
        @{ */

    /* NOTE: These functions are thread-safe in the same manner as described in the previous
       section, with the additional restriction that a stream cannot be called concurrently on
       multiple threads. */

    /** Opens a blob for reading, as a random-access byte stream. */
    C4ReadStream* c4blob_openReadStream(C4BlobStore*,
                                        C4BlobKey,
                                        C4Error* C4NULLABLE) C4API;

    /** Reads from an open stream.
        @param stream  The open stream to read from
        @param buffer  Where to copy the read data to
        @param maxBytesToRead  The maximum number of bytes to read to the buffer
        @param error  Error is returned here 
        @return  The actual number of bytes read, or 0 if an error occurred */
    size_t c4stream_read(C4ReadStream* stream,
                         void *buffer,
                         size_t maxBytesToRead,
                         C4Error* C4NULLABLE error) C4API;

    /** Returns the exact length in bytes of the stream. */
    int64_t c4stream_getLength(C4ReadStream*, C4Error* C4NULLABLE) C4API;

    /** Moves to a random location in the stream; the next c4stream_read call will read from that
        location. */
    bool c4stream_seek(C4ReadStream*,
                       uint64_t position,
                       C4Error* C4NULLABLE) C4API;

    /** Closes a read-stream. (A NULL parameter is allowed.) */
    void c4stream_close(C4ReadStream* C4NULLABLE) C4API;

    /** @} */


    /** \name Streamed Writes
        @{ */

    /** Opens a write stream for creating a new blob. You should then call c4stream_write to
        write the data, ending with c4stream_install to compute the blob's key and add it to
        the store, and then c4stream_closeWriter. */
    C4WriteStream* c4blob_openWriteStream(C4BlobStore*,
                                          C4Error* C4NULLABLE) C4API;

    /** Writes data to a stream. */
    bool c4stream_write(C4WriteStream*,
                        const void *bytes,
                        size_t length,
                        C4Error* C4NULLABLE) C4API;

    /** Returns the number of bytes written to the stream. */
    uint64_t c4stream_bytesWritten(C4WriteStream*) C4API;

    /** Computes the blob-key (digest) of the data written to the stream. This should only be
        called after writing the entire data. No more data can be written after this call. */
    C4BlobKey c4stream_computeBlobKey(C4WriteStream*) C4API;

    /** Adds the data written to the stream as a finished blob to the store.
        If `expectedKey` is not NULL, then the operation will fail unless the contents actually
        have that key. (If you don't know ahead of time what the key should be, call
        c4stream_computeBlobKey beforehand to derive it, and pass NULL for expectedKey.)
        This function does not close the writer. */
    bool c4stream_install(C4WriteStream*,
                          const C4BlobKey* C4NULLABLE expectedKey,
                          C4Error* C4NULLABLE) C4API;

    /** Closes a blob write-stream. If c4stream_install was not already called (or was called but
        failed), the temporary file will be deleted without adding the blob to the store. 
        (A NULL parameter is allowed, and is a no-op.) */
    void c4stream_closeWriter(C4WriteStream* C4NULLABLE) C4API;


    /** @} */
    /** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
