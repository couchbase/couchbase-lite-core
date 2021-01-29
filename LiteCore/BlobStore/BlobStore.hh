//
// BlobStore.hh
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
#include "Base.hh"
#include "FilePath.hh"
#include "Stream.hh"
#include "SecureDigest.hh"
#include <unordered_set>

namespace litecore {
    class BlobStore;
    class FilePath;


    /** A raw SHA-1 digest used as the unique identifier of a blob. */
    struct blobKey {
        SHA1 digest;

        blobKey() { }
        blobKey(slice rawBytes);

        static blobKey withBase64(slice base64, bool prefixed =true);

        bool readFromBase64(slice base64, bool prefixed =true);
        bool readFromFilename(std::string filename);

        operator slice() const          {return slice(digest);}
        std::string hexString() const   {return operator slice().hexString();}
        std::string base64String() const;
        std::string filename() const;

        bool operator== (const blobKey &k) const {
            return digest == k.digest;
        }
        bool operator!= (const blobKey &k) const {
            return !(*this == k);
        }

        static blobKey computeFrom(slice data);
    };


    /** Represents a blob stored in a BlobStore. This class is thread-safe. */
    class Blob {
    public:
        bool exists() const             {return _path.exists();}

        blobKey key() const             {return _key;}
        FilePath path() const           {return _path;}
        int64_t contentLength() const;      // An overestimate, if blob is encrypted

        alloc_slice contents() const    {return read()->readAll();}

        unique_ptr<SeekableReadStream> read() const;

        void del()                      {_path.del();}

    private:
        friend class BlobStore;
        friend class BlobWriteStream;
        
        Blob(const BlobStore&, const blobKey&);

        const FilePath _path;
        const blobKey _key;
        const BlobStore &_store;
    };


    /** A stream for writing a new Blob. */
    class BlobWriteStream : public WriteStream {
    public:
        BlobWriteStream(BlobStore&);
        ~BlobWriteStream();

        void write(slice) override;
        void close() override;

        uint64_t bytesWritten() const                       {return _bytesWritten;}

        /** Derives the blobKey from the digest of the file data.
            No more data can be written after this is called. */
        blobKey computeKey() noexcept;

        /** Adds the blob to the store and returns a Blob referring to it.
            No more data can be written after this is called.
            If expectedKey is not null, and doesn't match the actual computed key,
            a CorruptData exception is thrown. */
        Blob install(const blobKey *expectedKey =nullptr);

    private:
        BlobStore &_store;
        FilePath _tmpPath;
        shared_ptr<WriteStream> _writer;
        uint64_t _bytesWritten {0};
        SHA1Builder _sha1ctx;
        blobKey _key;
        bool _computedKey {false};
        bool _installed {false};
    };


    /** Manages a content-addressable store of binary blobs, stored as files in a directory.
        This class is thread-safe. */
    class BlobStore {
    public:
        struct Options {
            bool create         :1;     ///< Should the store be created if it doesn't exist?
            bool writeable      :1;     ///< If false, opened read-only
            EncryptionAlgorithm encryptionAlgorithm;
            alloc_slice encryptionKey;
            
            static const Options defaults;
        };

        BlobStore(const FilePath &dir, const Options* =nullptr);

        const FilePath& dir() const                 {return _dir;}
        const Options& options() const              {return _options;}
        bool isEncrypted() const                    {return _options.encryptionAlgorithm !=
                                                                kNoEncryption;}
        uint64_t count() const;
        uint64_t totalSize() const;

        void deleteStore()                          {_dir.delRecursive();}
        void deleteAllExcept(const std::unordered_set<std::string>& inUse);

        bool has(const blobKey &key) const          {return get(key).exists();}

        const Blob get(const blobKey &key) const    {return Blob(*this, key);}
        Blob get(const blobKey &key)                {return Blob(*this, key);}

        Blob put(slice data, const blobKey *expectedKey =nullptr);

        void copyBlobsTo(BlobStore &toStore);       // Copy my blobs into toStore
        void moveTo(BlobStore &toStore);            // Replace toStore's dir & options

    private:
        FilePath const  _dir;                           // Location
        Options         _options;                       // Option/capability flags
    };

}
