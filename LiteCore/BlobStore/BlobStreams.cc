//
// BlobStreams.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//


#include "BlobStreams.hh"
#include "EncryptedStream.hh"
#include "Error.hh"
#include "Logging.hh"

namespace litecore {
    using namespace std;
    using namespace fleece;


    LogDomain BlobLog("Blob");


#pragma mark - BLOB READ STREAM:

    unique_ptr<SeekableReadStream> OpenBlobReadStream(const FilePath &blobFile, EncryptionAlgorithm algorithm,
                                                      slice encryptionKey) {
        SeekableReadStream *reader = new FileReadStream(blobFile);
        if ( algorithm != EncryptionAlgorithm::kNoEncryption )
            reader = new EncryptedReadStream(shared_ptr<SeekableReadStream>(reader), algorithm, encryptionKey);
        return unique_ptr<SeekableReadStream>{reader};
    }

#pragma mark - BLOB WRITE STREAM:

    BlobWriteStream::BlobWriteStream(const string &blobsDir, EncryptionAlgorithm algorithm, slice encryptionKey) {
        FILE *file;
        _tmpPath = FilePath(blobsDir, "incoming_").mkTempFile(&file);
        _writer  = shared_ptr<WriteStream>{new FileWriteStream(file)};
        if ( algorithm != EncryptionAlgorithm::kNoEncryption )
            _writer = make_shared<EncryptedWriteStream>(_writer, algorithm, encryptionKey);
    }

    BlobWriteStream::~BlobWriteStream() {
        if ( !_installed ) deleteTempFile();
    }

    void BlobWriteStream::write(slice data) {
        Assert(!_blobKey, "Attempted to write after computing digest");
        _writer->write(data);
        _bytesWritten += data.size;
        _sha1ctx << data;
    }

    void BlobWriteStream::close() {
        if ( _writer ) {
            _writer->close();
            _writer = nullptr;
        }
    }

    C4BlobKey BlobWriteStream::computeKey() noexcept {
        if ( !_blobKey ) {
            _blobKey.emplace();
            _sha1ctx.finish(&_blobKey->bytes, sizeof(_blobKey->bytes));
        }
        return *_blobKey;
    }

    void BlobWriteStream::install(const FilePath &dstPath) {
        close();
        if ( !dstPath.exists() ) {
            _tmpPath.setReadOnly(true);
            _tmpPath.moveTo(dstPath);
        } else {
            // If the destination already exists, then this blob
            // already exists and doesn't need to be written again
            deleteTempFile();
        }
        _installed = true;
    }

    bool BlobWriteStream::deleteTempFile() {
        bool ok = false;
        try {
            ok = _tmpPath.del();
        } catch ( ... ) {}
        if ( !ok ) Warn("BlobWriteStream: unable to delete temporary file %s", _tmpPath.path().c_str());
        return ok;
    }

}  // namespace litecore
