//
// C4BlobStore.cc
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

#include "c4Internal.hh"
#include "c4BlobStore.h"
#include "c4Database.hh"
#include "BlobStore.hh"
using namespace std;


// This is a no-op class that just serves to make C4BlobStore type-compatible with BlobStore.
struct C4BlobStore : public BlobStore {
public:
    C4BlobStore(const FilePath &dirPath, const Options *options)
    :BlobStore(dirPath, options)
    { }
};


static inline const blobKey& asInternal(const C4BlobKey &key) {return *(blobKey*)&key;}
static inline const C4BlobKey& external(const blobKey &key) {return *(C4BlobKey*)&key;}
static inline const blobKey* asInternal(const C4BlobKey *key) {return (const blobKey*)key;}
static SeekableReadStream* asInternal(C4ReadStream* s)        {return (SeekableReadStream*)s;}
static inline C4ReadStream* external(SeekableReadStream* s) {return (C4ReadStream*)s;}
static BlobWriteStream* asInternal(C4WriteStream* s)          {return (BlobWriteStream*)s;}
static inline C4WriteStream* external(BlobWriteStream* s)   {return (C4WriteStream*)s;}


bool c4blob_keyFromString(C4Slice str, C4BlobKey* outKey) noexcept {
    try {
        if (!str.buf)
            return false;
        *outKey = external(blobKey::withBase64(str));
        return true;
    } catchExceptions()
    return false;
}


C4StringResult c4blob_keyToString(C4BlobKey key) noexcept {
    string str = asInternal(key).base64String();
    return stringResult(str);
}



C4BlobStore* c4blob_openStore(C4Slice dirPath,
                              C4DatabaseFlags flags,
                              const C4EncryptionKey *key,
                              C4Error* outError) noexcept
{
    try {
        BlobStore::Options options = {};
        options.create = (flags & kC4DB_Create) != 0;
        options.writeable = !(flags & kC4DB_ReadOnly);
        if (key) {
            options.encryptionAlgorithm = (EncryptionAlgorithm)key->algorithm;
            options.encryptionKey = alloc_slice(key->bytes, sizeof(key->bytes));
        }
        return new C4BlobStore(FilePath(toString(dirPath)), &options);
    } catchError(outError)
    return nullptr;
}


C4BlobStore* c4db_getBlobStore(C4Database *db, C4Error* outError) noexcept {
    try {
        return (C4BlobStore*) db->blobStore();
    } catchError(outError)
    return nullptr;
}


void c4blob_freeStore(C4BlobStore *store) noexcept {
    delete store;
}


bool c4blob_deleteStore(C4BlobStore* store, C4Error *outError) noexcept {
    try {
        store->deleteStore();
        delete store;
        return true;
    } catchError(outError)
    return false;
}


int64_t c4blob_getSize(C4BlobStore* store, C4BlobKey key) noexcept {
    try {
        return store->get(asInternal(key)).contentLength();
    } catchExceptions()
    return -1;
}


C4SliceResult c4blob_getContents(C4BlobStore* store, C4BlobKey key, C4Error* outError) noexcept {
    try {
        return C4SliceResult(store->get(asInternal(key)).contents());
    } catchError(outError)
    return {nullptr, 0};
}


C4StringResult c4blob_getFilePath(C4BlobStore* store, C4BlobKey key, C4Error* outError) noexcept {
    try {
        auto path = store->get(asInternal(key)).path();
        if (!path.exists()) {
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, {}, outError);
            return {nullptr, 0};
        } else if (store->isEncrypted()) {
            c4error_return(LiteCoreDomain, kC4ErrorWrongFormat, {}, outError);
            return {nullptr, 0};
        }
        return nullPaddedStringResult((string)path);
    } catchError(outError)
    return {nullptr, 0};
}


C4BlobKey c4blob_computeKey(C4Slice contents) {
    return external(blobKey::computeFrom(contents));
}


bool c4blob_create(C4BlobStore* store,
                   C4Slice contents,
                   const C4BlobKey *expectedKey,
                   C4BlobKey *outKey,
                   C4Error* outError) noexcept
{
    try {
        Blob blob = store->put(contents, asInternal(expectedKey));
        if (outKey)
            *outKey = external(blob.key());
        return true;
    } catchError(outError)
    return false;
}


bool c4blob_delete(C4BlobStore* store, C4BlobKey key, C4Error* outError) noexcept {
    try {
        store->get(asInternal(key)).del();
        return true;
    } catchError(outError)
    return false;
}


#pragma mark - STREAMING READS:


C4ReadStream* c4blob_openReadStream(C4BlobStore* store, C4BlobKey key, C4Error* outError) noexcept {
    try {
        unique_ptr<SeekableReadStream> stream = store->get(asInternal(key)).read();
        return external(stream.release());
    } catchError(outError)
    return nullptr;
}


size_t c4stream_read(C4ReadStream* stream, void *buffer, size_t maxBytes, C4Error* outError) noexcept {
    try {
        clearError(outError);
        return asInternal(stream)->read(buffer, maxBytes);
    } catchError(outError)
    return 0;
}


int64_t c4stream_getLength(C4ReadStream* stream, C4Error* outError) noexcept {
    try {
        return asInternal(stream)->getLength();
    } catchError(outError)
    return -1;
}


bool c4stream_seek(C4ReadStream* stream, uint64_t position, C4Error* outError) noexcept {
    try {
        asInternal(stream)->seek(position);
        return true;
    } catchError(outError)
    return false;
}


void c4stream_close(C4ReadStream* stream) noexcept {
    delete asInternal(stream);
}


#pragma mark - STREAMING WRITES:


C4WriteStream* c4blob_openWriteStream(C4BlobStore* store, C4Error* outError) noexcept {
    try {
        return external(new BlobWriteStream(*store));
    } catchError(outError)
    return nullptr;
}


bool c4stream_write(C4WriteStream* stream, const void *bytes, size_t length, C4Error* outError) noexcept {
    if (length == 0)
        return true;
    try {
        asInternal(stream)->write(slice(bytes, length));
        return true;
    } catchError(outError)
    return false;
}


uint64_t c4stream_bytesWritten(C4WriteStream* stream) {
    return asInternal(stream)->bytesWritten();
}


C4BlobKey c4stream_computeBlobKey(C4WriteStream* stream) noexcept {
    return external( asInternal(stream)->computeKey() );
}


bool c4stream_install(C4WriteStream* stream,
                      const C4BlobKey *expectedKey,
                      C4Error *outError) noexcept {
    try {
        asInternal(stream)->install(asInternal(expectedKey));
        return true;
    } catchError(outError)
    return false;
}


void c4stream_closeWriter(C4WriteStream* stream) noexcept {
    if (!stream)
        return;
    try {
        asInternal(stream)->close();
        delete asInternal(stream);
    } catchExceptions()
}
