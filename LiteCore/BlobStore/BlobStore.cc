//
//  BlobStore.cpp
//  LiteCore
//
//  Created by Jens Alfke on 8/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "BlobStore.hh"
#include "FilePath.hh"
#include "Error.hh"
#include "EncryptedStream.hh"
#include "Logging.hh"
#include <stdint.h>
#include <stdio.h>
#include <algorithm>

namespace litecore {
    using namespace std;

    LogDomain BlobLog("Blob");


#pragma mark - BLOBKEY:


    static constexpr size_t kBlobKeyStringLength = 5 + ((sizeof(blobKey::bytes) + 2) / 3) * 4;


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


#pragma mark - BLOB READING:
    
    
    Blob::Blob(const BlobStore &store, const blobKey &key)
    :_path(store.dir(), key.filename()),
     _key(key),
     _store(store)
    { }


    int64_t Blob::contentLength() const {
        int64_t length = path().dataSize();
        if (length >= 0 && _store.options().encryptionAlgorithm != kNoEncryption)
            length -= EncryptedReadStream::kFileSizeOverhead;
        return length;
    }



    unique_ptr<SeekableReadStream> Blob::read() const {
        SeekableReadStream *reader = new FileReadStream(_path);
        auto &options = _store.options();
        if (options.encryptionAlgorithm != kNoEncryption) {
            reader = new EncryptedReadStream(shared_ptr<SeekableReadStream>(reader),
                                             options.encryptionAlgorithm,
                                             options.encryptionKey);
        }
        return unique_ptr<SeekableReadStream>{reader};
    }


#pragma mark - BLOB WRITING:


    BlobWriteStream::BlobWriteStream(BlobStore &store)
    :_store(store)
    {
        FILE *file;
        _tmpPath = store.dir()["incoming_"].mkTempFile("_~", &file);
        _writer = shared_ptr<WriteStream> {new FileWriteStream(file)};
        auto &options = _store.options();
        if (options.encryptionAlgorithm != kNoEncryption) {
            _writer = shared_ptr<WriteStream> {new EncryptedWriteStream(_writer,
                                                                    options.encryptionAlgorithm,
                                                                    options.encryptionKey)};
        }
        sha1_begin(&_sha1ctx);
    }


    BlobWriteStream::~BlobWriteStream() {
        if (!_installed) {
            try {
                _tmpPath.del();
            } catch (...) {
                // destructor is not allowed to throw exceptions
                Warn("BlobWriteStream: unable to delete temporary file %s",
                     _tmpPath.path().c_str());
            }
        }
    }


    void BlobWriteStream::write(slice data) {
        Assert(!_computedKey, "Attempted to write after computing digest");
        _writer->write(data);
        sha1_add(&_sha1ctx, data.buf, data.size);
    }

    void BlobWriteStream::close() {
        if (_writer) {
            _writer->close();
            _writer = nullptr;
        }
    }

    blobKey BlobWriteStream::computeKey() noexcept {
        if (!_computedKey) {
            sha1_end(&_sha1ctx, &_key.bytes);
            _computedKey = true;
        }
        return _key;
    }


    Blob BlobWriteStream::install() {
        close();
        Blob blob(_store, computeKey());
        _tmpPath.setReadOnly(true);
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
        if (_dir.exists()) {
            _dir.mustExistAsDir();
        } else {
            if (!_options.create)
                error::_throw(error::NotFound);
            _dir.mkdir();
        }
    }


    Blob BlobStore::put(slice data) {
        BlobWriteStream stream(*this);
        stream.write(data);
        return stream.install();
    }

}
