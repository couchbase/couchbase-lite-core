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

    /** A raw SHA-1 digest used as the unique identifier of a blob. */
    typedef struct C4BlobKey {
        uint8_t bytes[20];
    } C4BlobKey;

    
    /** Decodes a string of the form "sha1-"+base64 into a raw key. */
    bool c4blob_keyFromString(C4Slice str, C4BlobKey*);

    /** Encodes a blob key to a string of the form "sha1-"+base64. */
    C4SliceResult c4blob_keyToString(C4BlobKey);



    /** Opaque handle for an object that manages storage of blobs. */
    typedef struct c4BlobStore C4BlobStore;


    /** Opens a BlobStore in a directory. If the flags allow creating, the directory will be
        created if necessary. */
    C4BlobStore* c4blob_openStore(C4Slice dirPath,
                                  C4DatabaseFlags,
                                  const C4EncryptionKey*,
                                  C4Error*);

    /** Closes/frees a BlobStore. */
    void c4blob_freeStore(C4BlobStore*);

    /** Deletes the BlobStore's blobs and directory, and (if successful) frees the object. */
    bool c4blob_deleteStore(C4BlobStore*, C4Error*);


    /** Gets the content size of a blob given its key. Returns -1 if it doesn't exist. */
    int64_t c4blob_getSize(C4BlobStore*, C4BlobKey);

    /** Reads the entire contents of a blob into memory. Caller is responsible for freeing it. */
    C4SliceResult c4blob_getContents(C4BlobStore*, C4BlobKey, C4Error*);


    /** Stores a blob. The associated key will be written to `outKey`. */
    bool c4blob_create(C4BlobStore*, C4Slice contents, C4BlobKey *outKey, C4Error*);


    /** Deletes a blob from the store given its key. */
    bool c4blob_delete(C4BlobStore*, C4BlobKey, C4Error*);


#if 0
    // STREAMING API:

    typedef struct c4Stream C4Stream;

    C4Stream* c4blob_open(C4BlobStore*, C4BlobKey, C4Error*);

    C4Stream* c4blob_createWithStream(C4BlobStore*, C4Error*);

    size_t c4stream_read(C4Stream*, void *buffer, size_t maxBytes, C4Error*);

    bool c4stream_write(C4Stream*, const void *bytes, size_t length, C4Error*);

    C4BlobKey c4stream_getBlobKey(C4Stream*);

    bool c4stream_close(C4Stream*, C4Error*);

    void c4Stream_cancel(C4Stream*);
#endif


#ifdef __cplusplus
}
#endif
