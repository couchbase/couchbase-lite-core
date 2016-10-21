//
//  c4BlobStore.h
//  LiteCore
//
//  Created by Jens Alfke on 9/1/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "c4Database.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

    /** \defgroup Blobs Blobs
        @{ */

    //////// BLOB KEYS:

    /** \name Blob Keys
        @{ */

    /** A raw SHA-1 digest used as the unique identifier of a blob. */
    typedef struct C4BlobKey {
        uint8_t bytes[20];
    } C4BlobKey;

    
    /** Decodes a string of the form "sha1-"+base64 into a raw key. */
    bool c4blob_keyFromString(C4Slice str, C4BlobKey*) C4API;

    /** Encodes a blob key to a string of the form "sha1-"+base64. */
    C4SliceResult c4blob_keyToString(C4BlobKey) C4API;


    /** Opaque handle for an object that manages storage of blobs. */
    typedef struct c4BlobStore C4BlobStore;

    /** @} */


    //////// BLOB STORE API:

    /** \name Blob Store API
        @{ */

    /** Opens a BlobStore in a directory. If the flags allow creating, the directory will be
        created if necessary.
        @param dirPath  The filesystem path of the directory holding the attachments.
        @param flags  Specifies options like create, read-only
        @param encryptionKey  Optional encryption algorithm & key
        @param outError  Error is returned here
        @return  The BlobStore reference, or NULL on error */
    C4BlobStore* c4blob_openStore(C4Slice dirPath,
                                  C4DatabaseFlags flags,
                                  const C4EncryptionKey* encryptionKey,
                                  C4Error* outError) C4API;

    /** Closes/frees a BlobStore. (A NULL parameter is allowed.) */
    void c4blob_freeStore(C4BlobStore*) C4API;

    /** Deletes the BlobStore's blobs and directory, and (if successful) frees the object. */
    bool c4blob_deleteStore(C4BlobStore*, C4Error*) C4API;

    /** @} */


    /** \name Blob API
        @{ */

    /** Gets the content size of a blob given its key. Returns -1 if it doesn't exist.
        WARNING: If the blob is encrypted, the return value is a conservative estimate that may
        be up to 16 bytes larger than the actual size. */
    int64_t c4blob_getSize(C4BlobStore*, C4BlobKey) C4API;

    /** Reads the entire contents of a blob into memory. Caller is responsible for freeing it. */
    C4SliceResult c4blob_getContents(C4BlobStore*, C4BlobKey, C4Error*) C4API;

    /** Stores a blob. The associated key will be written to `outKey`. */
    bool c4blob_create(C4BlobStore*, C4Slice contents, C4BlobKey *outKey, C4Error*) C4API;


    /** Deletes a blob from the store given its key. */
    bool c4blob_delete(C4BlobStore*, C4BlobKey, C4Error*) C4API;

    /** @} */


    //////// STREAMING API:

    /** \name Streamed Reads
        @{ */

    /** An open stream for reading data from a blob. */
    typedef struct c4ReadStream C4ReadStream;

    /** Opens a blob for reading, as a random-access byte stream. */
    C4ReadStream* c4blob_openReadStream(C4BlobStore*, C4BlobKey, C4Error*) C4API;

    /** Reads from an open stream.
        @param stream  The open stream to read from
        @param buffer  Where to copy the read data to
        @param maxBytesToRead  The maximum number of bytes to read to the buffer
        @param error  Error is returned here 
        @return  The actual number of bytes read, or 0 if an error occurred */
    size_t c4stream_read(C4ReadStream* stream,
                         void *buffer,
                         size_t maxBytesToRead,
                         C4Error* error) C4API;

    /** Returns the exact length in bytes of the stream. */
    int64_t c4stream_getLength(C4ReadStream*, C4Error*) C4API;

    /** Moves to a random location in the stream; the next c4stream_read call will read from that
        location. */
    bool c4stream_seek(C4ReadStream*,
                       uint64_t position,
                       C4Error*) C4API;

    /** Closes a read-stream. (A NULL parameter is allowed.) */
    void c4stream_close(C4ReadStream*) C4API;

    /** @} */


    /** \name Streamed Writes
        @{ */

    /** An open stream for writing data to a blob. */
    typedef struct c4WriteStream C4WriteStream;

    /** Opens a write stream for creating a new blob. You should then call c4stream_write to
        write the data, ending with c4stream_install to compute the blob's key and add it to
        the store, and then c4stream_closeWriter. */
    C4WriteStream* c4blob_openWriteStream(C4BlobStore*, C4Error*) C4API;

    /** Writes data to a stream. */
    bool c4stream_write(C4WriteStream*, const void *bytes, size_t length, C4Error*) C4API;

    /** Computes the blob-key (digest) of the data written to the stream. This should only be
        called after writing the entire data. No more data can be written after this call. */
    C4BlobKey c4stream_computeBlobKey(C4WriteStream*) C4API;

    /** Adds the data written to the stream as a finished blob to the store, and returns its key.
        If you skip this call, the blob will not be added to the store. (You might do this if you
        were unable to receive all of the data from the network, or if you've called
        c4stream_computeBlobKey and found that the data does not match the expected digest/key.) */
    bool c4stream_install(C4WriteStream*, C4Error*) C4API;

    /** Closes a blob write-stream. If c4stream_install was not already called, the temporary file
        will be deleted without adding the blob to the store. (A NULL parameter is allowed.) */
    void c4stream_closeWriter(C4WriteStream*) C4API;


    /** @} */
    /** @} */

#ifdef __cplusplus
}
#endif
