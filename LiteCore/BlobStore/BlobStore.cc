//
//  BlobStore.cpp
//  LiteCore
//
//  Created by Jens Alfke on 8/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "BlobStore.hh"
#include "FilePath.hh"
#include "Error.hh"
#include "Writer.hh"
#include <stdint.h>
#include <stdio.h>


namespace litecore {
    using namespace std;


#pragma mark - BLOBKEY:


    static const size_t kBlobKeyStringLength = 5 + ((sizeof(blobKey::bytes) + 2) / 3) * 4;


    blobKey::blobKey(slice s) {
        if (s.size != sizeof(bytes))
            error::_throw(error::WrongFormat);
        memcpy(bytes, s.buf, sizeof(bytes));
    }

    blobKey::blobKey(const string &str) {
        slice data = slice(str);
        if (data.size == kBlobKeyStringLength && 0 == memcmp(data.buf, "sha1-", 5)) {
            data.moveStart(5);
            // Decoder always writes a multiple of 3 bytes, so round up:
            uint8_t buf[21];
            slice result = data.readBase64Into(slice(buf, sizeof(buf)));
            if (result.size == 20) {
                memcpy(bytes, result.buf, result.size);
                return;
            }
        }
        error::_throw(error::WrongFormat);
    }


    string blobKey::base64String() const {
        return string("sha1-") + slice(bytes, sizeof(bytes)).base64String();
    }


    string blobKey::filename() const {
        string str = slice(bytes, sizeof(bytes)).base64String();
        replace(str.begin(), str.end(), '/', '_');
        return str + ".blob";
    }


    /*static*/ blobKey blobKey::computeFrom(slice data) {
#if SECURE_DIGEST_AVAILABLE
        blobKey key;
        sha1Context ctx;
        sha1_begin(&ctx);
        sha1_add(&ctx, data.buf, data.size);
        sha1_end(&ctx, &key.bytes);
        return key;
#else
        error::_throw(error::Unimplemented);
#endif
    }


#pragma mark - BLOB:
    
    
    Blob::Blob(const BlobStore &store, const blobKey &key)
    :_path(store.dir(), key.filename()),
     _key(key)
    { }


    static void checkErr(FILE *file) {
        int err = ferror(file);
        if (err)
            error::_throw(error::POSIX, err);
    }

    class BlobReadStream : public ReadStream {
    public:
        BlobReadStream(const Blob &blob) {
            _file = fopen(blob.path().path().c_str(), "r");
            if (!_file)
                error::_throwErrno();
        }

        ~BlobReadStream() {
            if (_file)
                fclose(_file);
        }

        bool atEOF() const override {
            return feof(_file) != 0;
        }

        uint64_t getLength() const override {
            uint64_t curPos = ftell(_file);
            fseek(_file, 0, SEEK_END);
            uint64_t fileSize = ftell(_file);
            fseek(_file, curPos, SEEK_SET);
            checkErr(_file);
            return fileSize;
        }

        void seek(uint64_t pos) override {
            fseek(_file, pos, SEEK_SET);
            checkErr(_file);
        }

        size_t read(void *dst, size_t count) override {
            size_t bytesRead = fread(dst, 1, count, _file);
            checkErr(_file);
            return bytesRead;
        }

    private:
        FILE* _file {nullptr};
    };


    alloc_slice ReadStream::readAll() {
        uint64_t length = getLength();
        if (length > SIZE_MAX)    // overflow check for 32-bit
            throw bad_alloc();
        auto contents = alloc_slice((size_t)length);
        contents.size = read((void*)contents.buf, length);
        return contents;
    }


    unique_ptr<ReadStream> Blob::read() const {
        return unique_ptr<ReadStream>{ new BlobReadStream(*this) };
    }


#pragma mark - BLOB WRITING:


    BlobWriteStream::BlobWriteStream(BlobStore &store)
    :_store(store)
    {
        _tmpPath = store.dir()["incoming_"].mkTempFile("tmp", &_file);
        sha1_begin(&_sha1ctx);
    }


    BlobWriteStream::~BlobWriteStream() {
        if (_file)
            fclose(_file);
        if (!_installed)
            _tmpPath.del();
    }


    BlobWriteStream& BlobWriteStream::write(slice data) {
        CBFAssert(!_computedKey);
        if (fwrite(data.buf, 1, data.size, _file) < data.size)
            checkErr(_file);
        sha1_add(&_sha1ctx, data.buf, data.size);
        return *this;
    }

    blobKey BlobWriteStream::computeKey() {
        if (!_computedKey) {
            sha1_end(&_sha1ctx, &_key.bytes);
            _computedKey = true;
        }
        return _key;
    }


    Blob BlobWriteStream::install() {
        fclose(_file);
        _file = nullptr;
        Blob blob(_store, computeKey());
        _tmpPath.moveTo(blob.path());
        _installed = true;
        return blob;
    }


#pragma mark - BLOBSTORE:


    const BlobStore::Options BlobStore::Options::defaults = {true, true};


    BlobStore::BlobStore(const FilePath &dir, const Options *options)
    :_dir(dir),
     _options(options ? *options : Options::defaults)
    {
        if (_options.encryptionAlgorithm != kNoEncryption)
            error::_throw(error::UnsupportedEncryption);        //TODO: Implement encryption

        if (_dir.exists()) {
            _dir.mustExistAsDir();
        } else {
            if (!_options.create)
                error::_throw(error::NotFound);
            _dir.mkdir();
        }
    }

}
