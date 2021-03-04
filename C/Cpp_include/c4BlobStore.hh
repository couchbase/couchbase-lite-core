//
// c4BlobStore.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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
#include "c4Base.hh"
#include "c4BlobStore.h"
#include "fleece/Fleece.h"
#include <memory>

namespace litecore {
    class BlobStore;
    class BlobWriteStream;
    class SeekableReadStream;
}
namespace c4Internal {
    class Database;
}

C4_ASSUME_NONNULL_BEGIN



struct C4ReadStream : public C4Base {
    C4ReadStream(const C4BlobStore&, C4BlobKey);
    ~C4ReadStream();
    size_t read(void *buffer, size_t maxBytes);
    int64_t length() const;
    void seek(int64_t pos);
private:
    std::unique_ptr<litecore::SeekableReadStream> _impl;
};


struct C4WriteStream : public C4Base {
    explicit C4WriteStream(C4BlobStore&);
    ~C4WriteStream();
    void write(slice);
    uint64_t bytesWritten() const noexcept;
    C4BlobKey computeBlobKey();
    void install(const C4BlobKey* C4NULLABLE expectedKey =nullptr);
private:
    std::unique_ptr<litecore::BlobWriteStream> _impl;
};


struct C4BlobStore : public C4Base {
    // NOTE: Usually accessed via database->blobStore().

    static C4BlobKey computeKey(slice contents) noexcept;

    static alloc_slice keyToString(C4BlobKey key);
    static bool keyFromString(slice str, C4BlobKey* C4NULLABLE outKey) noexcept; //?? return optional<C4BlobKey>?

    ~C4BlobStore();

    void deleteStore();

    int64_t getSize(C4BlobKey) const;
    alloc_slice getContents(C4BlobKey) const;

    /// The filesystem path of a blob, or nullslice if no blob with that key exists.
    alloc_slice getFilePath(C4BlobKey) const;

    C4BlobKey createBlob(slice contents, const C4BlobKey* C4NULLABLE expectedKey =nullptr);
    void deleteBlob(C4BlobKey);

    C4ReadStream openReadStream(C4BlobKey key) const    {return C4ReadStream(*this, key);}
    C4WriteStream openWriteStream(C4BlobKey key)        {return C4WriteStream(*this);}

    /** Returns the contents of a blob referenced by a dict. Inline data will be decoded if
         necessary, or the "digest" property will be looked up in the BlobStore if one is
         provided.
         Returns a null slice if the blob data is not inline but no BlobStore is given.
         Otherwise throws an exception if it's unable to return data. */
    alloc_slice getBlobData(FLDict dict);

    // Static utility functions for blob dicts:

    /** Returns the dict's "digest" property decoded into a blobKey. */
    static bool getBlobKey(FLDict dict, C4BlobKey &outKey);

    /** Returns true if the given dictionary is a [reference to a] blob. */
    static bool dictIsBlob(FLDict dict);

    /** Returns true if the given dictionary is a [reference to a] blob; if so, gets its key. */
    static bool dictIsBlob(FLDict dict, C4BlobKey &outKey);

    /** Returns true if the blob dictionary's data type appears to be compressible. */
    static bool blobIsCompressible(FLDict);

// rarely used / for testing only:
    C4BlobStore(slice dirPath,
                C4DatabaseFlags,
                const C4EncryptionKey* C4NULLABLE = nullptr);

protected:
    friend struct C4ReadStream;
    friend struct C4WriteStream;
    friend struct C4Database;
    friend class c4Internal::Database;

    C4BlobStore(litecore::BlobStore *store) :_impl(store) { }
    static alloc_slice getBlobData(FLDict dict, litecore::BlobStore *store);

private:
    litecore::BlobStore* C4NULLABLE _impl;
    bool const _ownsImpl {false};
};

C4_ASSUME_NONNULL_END
