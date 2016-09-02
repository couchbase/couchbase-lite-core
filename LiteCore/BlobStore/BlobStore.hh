//
//  BlobStore.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "FilePath.hh"


namespace litecore {
    class BlobStore;
    class FilePath;


    /** A raw SHA-1 digest used as the unique identifier of a blob. */
    struct blobKey {
        uint8_t bytes[20];

        blobKey() { }
        blobKey(slice);
        blobKey(const std::string &base64);

        operator slice() const          {return slice(bytes, sizeof(bytes));}
        std::string hexString() const   {return operator slice().hexString();}
        std::string base64String() const;
        std::string filename() const;

        static blobKey computeFrom(slice data);
    };


    /** A simple read-only stream interface. */
    class ReadStream {
    public:
        virtual ~ReadStream() = default;
        virtual bool atEOF() const =0;
        virtual uint64_t getLength() const =0;
        virtual size_t read(void *dst, size_t count) =0;
        virtual void seek(uint64_t pos) =0;
        alloc_slice readAll();
    };


    /** Represents a blob stored in a BlobStore. */
    class Blob {
    public:
        bool exists() const             {return _path.exists();}

        blobKey key() const             {return _key;}
        FilePath path() const           {return _path;}
        int64_t contentLength() const   {return path().dataSize();}

        alloc_slice contents() const    {return read()->readAll();}

        std::unique_ptr<ReadStream> read() const;

        void del()                      {_path.del();}

    private:
        friend class BlobStore;
        Blob(const BlobStore&, const blobKey&);

        FilePath _path;
        const blobKey _key;
    };


    /** Manages a content-addressable store of binary blobs, stored as files in a directory. */
    class BlobStore {
    public:
        struct Options {
            bool create         :1;     //< Should the store be created if it doesn't exist?
            bool writeable      :1;     //< If false, opened read-only
            EncryptionAlgorithm encryptionAlgorithm;
            alloc_slice encryptionKey;
            static const Options defaults;
        };

        BlobStore(const FilePath &dir, const Options* =nullptr);

        const FilePath& dir() const                 {return _dir;}
        const Options& options() const              {return _options;}
        uint64_t count() const;
        uint64_t totalSize() const;

        void deleteStore()                          {_dir.delRecursive();}

        bool has(const blobKey &key) const          {return get(key).exists();}

        const Blob get(const blobKey &key) const    {return Blob(*this, key);}
        Blob get(const blobKey &key)                {return Blob(*this, key);}

        Blob put(slice data);

    private:
        FilePath const          _dir;                           // Location
        Options                 _options;                       // Option/capability flags
    };

}
