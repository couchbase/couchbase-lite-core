//
// c4BlobStore.h
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
#include "c4BlobStoreTypes.h"
#include "c4DatabaseTypes.h"
#ifdef __cplusplus
#    include <cstdio>
#else
#    include <stdio.h>
#endif

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

/** \defgroup Blobs Blobs
        @{ */

/** \name Blob Keys
        @{ */


/** Decodes a string of the form "sha1-"+base64 into a raw key.
    \note This function is thread-safe. */
CBL_CORE_API bool c4blob_keyFromString(C4String str, C4BlobKey*) C4API;

/** Encodes a blob key to a string of the form "sha1-"+base64.
    \note This function is thread-safe. */
CBL_CORE_API C4StringResult c4blob_keyToString(C4BlobKey) C4API;

/** @} */


//////// BLOB STORE API:

/** \name Blob Store API
        @{ */

/** Returns the BlobStore associated with a bundled database.
        \note The caller must use a lock for Database when this function is called.
 
        Fails if the database is not bundled.
        DO NOT call c4blob_freeStore on this! The C4Database will free it when it closes. */
CBL_CORE_API C4BlobStore* c4db_getBlobStore(C4Database* db, C4Error* C4NULLABLE outError) C4API;

/** Opens a BlobStore in a directory. If the flags allow creating, the directory will be
        created if necessary.
        Call c4blob_freeStore() when finished using the BlobStore.
        \note This function is thread-safe.
        \warning This should only be used for unit testing. Naked BlobStores are not supported.
        @param dirPath  The filesystem path of the directory holding the attachments.
        @param flags  Specifies options like create, read-only
        @param encryptionKey  Optional encryption algorithm & key
        @param outError  Error is returned here
        @return  The BlobStore reference, or NULL on error */
NODISCARD CBL_CORE_API C4BlobStore* c4blob_openStore(C4String dirPath, C4DatabaseFlags flags,
                                                     const C4EncryptionKey* C4NULLABLE encryptionKey,
                                                     C4Error* C4NULLABLE               outError) C4API;

/** Closes/frees a BlobStore. (A NULL parameter is allowed.)
        \note This function is thread-safe.
        \warning This should only be used for unit testing. Never free a BlobStore belonging to a
                 C4Database. */
CBL_CORE_API void c4blob_freeStore(C4BlobStore* C4NULLABLE) C4API;

/** Deletes the BlobStore's blobs and directory, and (if successful) frees the object.
        \note This function is thread-safe.
        \warning This should only be used for unit testing. Never delete a BlobStore belonging to a
                 C4Database. */
CBL_CORE_API bool c4blob_deleteStore(C4BlobStore*, C4Error* C4NULLABLE) C4API;

/** @} */


/** \name Blob API
        @{ */

/* NOTE: Every function in this section is thread-safe, as long as the C4BlobStore
       reference remains valid while the function executes, i.e. there are no concurrent calls
       to c4blob_freeStore, c4blob_deleteStore or c4db_close. */

/** Gets the content size of a blob given its key. Returns -1 if it doesn't exist.
        \note This function is thread-safe.
        
        WARNING: If the blob is encrypted, the return value is a conservative estimate that may
        be up to 16 bytes larger than the actual size. */
CBL_CORE_API int64_t c4blob_getSize(C4BlobStore*, C4BlobKey) C4API;

/** Reads the entire contents of a blob into memory. Caller is responsible for freeing it. */
CBL_CORE_API C4SliceResult c4blob_getContents(C4BlobStore*, C4BlobKey, C4Error* C4NULLABLE) C4API;

/** Returns the path of the file that stores the blob, if possible. This call may fail with
        error kC4ErrorWrongFormat if the blob is encrypted (in which case the file would be
        unreadable by the caller) or with kC4ErrorUnsupported if for some implementation reason
        the blob isn't stored as a standalone file.
        Thus, the caller MUST use this function only as an optimization, and fall back to reading
        the contents via the API if it fails.
        Also, it goes without saying that the caller MUST not modify the file! 
        \note This function is thread-safe. */
CBL_CORE_API C4StringResult c4blob_getFilePath(C4BlobStore*, C4BlobKey, C4Error* C4NULLABLE) C4API;

/** Derives the key of the given data, without storing it. */
NODISCARD CBL_CORE_API C4BlobKey c4blob_computeKey(C4Slice contents);

/** Stores a blob. The associated key will be written to `outKey`, if non-NULL.
        If `expectedKey` is not NULL, then the operation will fail unless the contents actually
        have that key. */
NODISCARD CBL_CORE_API bool c4blob_create(C4BlobStore* store, C4Slice contents, const C4BlobKey* C4NULLABLE expectedKey,
                                          C4BlobKey* C4NULLABLE outKey, C4Error* C4NULLABLE error) C4API;

/** Deletes a blob from the store given its key. */
NODISCARD CBL_CORE_API bool c4blob_delete(C4BlobStore*, C4BlobKey, C4Error* C4NULLABLE) C4API;

/** @} */


//////// STREAMING API:

/** \name Streamed Reads
        @{ */

/* NOTE: These functions are thread-safe in the same manner as described in the previous
       section, with the additional restriction that a stream cannot be called concurrently on
       multiple threads. */

/** Opens a blob for reading, as a random-access byte stream.
    \note This function is thread-safe. */
NODISCARD CBL_CORE_API C4ReadStream* c4blob_openReadStream(C4BlobStore*, C4BlobKey, C4Error* C4NULLABLE) C4API;

/** Reads from an open stream.
        \note The caller must use a lock for ReadStream when this function is called.
        @param stream  The open stream to read from
        @param buffer  Where to copy the read data to
        @param maxBytesToRead  The maximum number of bytes to read to the buffer
        @param error  Error is returned here 
        @return  The actual number of bytes read, or 0 if an error occurred */
NODISCARD CBL_CORE_API size_t c4stream_read(C4ReadStream* stream, void* buffer, size_t maxBytesToRead,
                                            C4Error* C4NULLABLE error) C4API;

/** Returns the exact length in bytes of the stream. 
    \note This function is thread-safe. */
CBL_CORE_API int64_t c4stream_getLength(C4ReadStream*, C4Error* C4NULLABLE) C4API;

/** Moves to a random location in the stream; the next c4stream_read call will read from that
        location. 
    \note The caller must use a lock for ReadStram when this function is called. */
NODISCARD CBL_CORE_API bool c4stream_seek(C4ReadStream*, uint64_t position, C4Error* C4NULLABLE) C4API;

/** @} */


/** \name Streamed Writes
        @{ */

/** Opens a write stream for creating a new blob. You should then call c4stream_write to
        write the data, ending with c4stream_install to compute the blob's key and add it to
        the store, and then c4stream_closeWriter. 
    \note This function is thread-safe. */
NODISCARD CBL_CORE_API C4WriteStream* c4blob_openWriteStream(C4BlobStore*, C4Error* C4NULLABLE) C4API;

/** Writes data to a stream. 
    \note The caller must use a lock for WriteStream when this function is called. */
NODISCARD CBL_CORE_API bool c4stream_write(C4WriteStream*, const void* bytes, size_t length, C4Error* C4NULLABLE) C4API;

/** Returns the number of bytes written to the stream.
    \note The caller must use a lock for WriteStream when this function is called.*/
CBL_CORE_API uint64_t c4stream_bytesWritten(C4WriteStream*) C4API;

/** Computes the blob-key (digest) of the data written to the stream. This should only be
        called after writing the entire data. No more data can be written after this call. 
    \note The caller must use a lock for WriteStream when this function is called. */
NODISCARD CBL_CORE_API C4BlobKey c4stream_computeBlobKey(C4WriteStream*) C4API;

/** Adds the data written to the stream as a finished blob to the store.
        If `expectedKey` is not NULL, then the operation will fail unless the contents actually
        have that key. (If you don't know ahead of time what the key should be, call
        c4stream_computeBlobKey beforehand to derive it, and pass NULL for expectedKey.)
        This function does not close the writer. 
    \note The caller must use a lock for WriteStream when this function is called. */
NODISCARD CBL_CORE_API bool c4stream_install(C4WriteStream*, const C4BlobKey* C4NULLABLE expectedKey,
                                             C4Error* C4NULLABLE) C4API;


/** @} */
/** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
