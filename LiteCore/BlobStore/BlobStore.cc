//
//  BlobStore.cc
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
//.

#include "BlobStore.hh"
#include "FilePath.hh"
#include "Error.hh"
#include "EncryptedStream.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <stdint.h>
#include <stdio.h>
#include <algorithm>

namespace litecore {
    using namespace std;
    using namespace fleece;

    LogDomain BlobLog("Blob");


#pragma mark - BLOBKEY:


    static constexpr size_t kBlobKeyStringLength = ((sizeof(blobKey::digest) + 2) / 3) * 4;


    blobKey::blobKey(slice s) {
        if (!digest.setDigest(s))
            error::_throw(error::WrongFormat);
    }

    blobKey blobKey::withBase64(slice base64, bool prefixed) {
        blobKey key;
        if (!key.readFromBase64(base64, prefixed))
            error::_throw(error::WrongFormat);
        return key;
    }


    bool blobKey::readFromBase64(slice data, bool prefixed) {
        if (prefixed) {
            if (data.hasPrefix("sha1-"_sl))
                data.moveStart(5);
            else
                return false;
        }
        if (data.size == kBlobKeyStringLength) {
            // Decoder always writes a multiple of 3 bytes, so round up:
            uint8_t buf[21];
            slice result = data.readBase64Into(slice(buf, sizeof(buf)));
            return digest.setDigest(result);
        }
        return false;
    }


    string blobKey::base64String() const {
        return string("sha1-") + slice(digest).base64String();
    }


    string blobKey::filename() const {
        string str = slice(digest).base64String();
        replace(str.begin(), str.end(), '/', '_');
        return str + ".blob";
    }


    bool blobKey::readFromFilename(string filename) {
        if (!hasSuffix(filename, ".blob"))
            return false;
        filename.resize(filename.size() - 5);
        replace(filename.begin(), filename.end(), '_', '/');
        return readFromBase64(slice(filename), false);
    }


    /*static*/ blobKey blobKey::computeFrom(slice data) {
        blobKey key;
        key.digest.computeFrom(data);
        return key;
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
        _tmpPath = store.dir()["incoming_"].mkTempFile(&file);
        _writer = shared_ptr<WriteStream> {new FileWriteStream(file)};
        auto &options = _store.options();
        if (options.encryptionAlgorithm != kNoEncryption) {
            _writer = make_shared<EncryptedWriteStream>(_writer,
                                                        options.encryptionAlgorithm,
                                                        options.encryptionKey);
        }
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
        _bytesWritten += data.size;
        _sha1ctx << data;
    }

    void BlobWriteStream::close() {
        if (_writer) {
            _writer->close();
            _writer = nullptr;
        }
    }

    blobKey BlobWriteStream::computeKey() noexcept {
        if (!_computedKey) {
            _key.digest = _sha1ctx.finish();
            _computedKey = true;
        }
        return _key;
    }


    Blob BlobWriteStream::install(const blobKey *expectedKey) {
        close();
        auto key = computeKey();
        if (expectedKey && *expectedKey != key)
            error::_throw(error::CorruptData);
        Blob blob(_store, key);
        if(!blob.path().exists()) {
            _tmpPath.setReadOnly(true);
            _tmpPath.moveTo(blob.path());
        } else {
            // If the destination already exists, then this blob
            // already exists and doesn't need to be written again
            if(!_tmpPath.del()) {
                string tmpPath = _tmpPath.path();
                Warn("Unable to delete temporary blob %s", tmpPath.c_str());
            }
        }

        _installed = true;
        return blob;
    }
    
#pragma mark - DELETING:
    
    void BlobStore::deleteAllExcept(const unordered_set<string> &inUse) {
        _dir.forEachFile([&inUse](const FilePath &path) {
            if(find(inUse.cbegin(), inUse.cend(), path.fileName()) == inUse.cend()) {
                path.del();
            }
        });
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


    Blob BlobStore::put(slice data, const blobKey *expectedKey) {
        BlobWriteStream stream(*this);
        stream.write(data);
        return stream.install(expectedKey);
    }


    void BlobStore::copyBlobsTo(BlobStore &toStore) {
        _dir.forEachFile([&](const FilePath &path) {
            blobKey key;
            if (!key.readFromFilename(path.fileName()))
                return;
            Blob srcBlob(*this, key);
            auto src = srcBlob.read();
            BlobWriteStream dst(toStore);
            uint8_t buffer[4096];
            size_t bytesRead;
            while ((bytesRead = src->read(buffer, sizeof(buffer))) > 0) {
                dst.write(slice(buffer, bytesRead));
            }
            dst.install(&key);
        });
    }


    void BlobStore::moveTo(BlobStore &toStore) {
        _dir.moveToReplacingDir(toStore.dir(), true);
        toStore._options = _options;
    }

}
