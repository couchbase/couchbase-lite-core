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

#pragma once
#include "c4BlobStoreTypes.h"
#include "FilePath.hh"
#include "SecureDigest.hh"
#include "Stream.hh"
#include <memory>
#include <optional>
#include <string>

namespace litecore {

    /** Returns a stream for reading a blob from the given file in the BlobStore. */
    unique_ptr<SeekableReadStream> OpenBlobReadStream(const FilePath& blobFile, EncryptionAlgorithm,
                                                      slice           encryptionKey);

    /** A stream for writing a new blob. */
    class BlobWriteStream final : public WriteStream {
      public:
        BlobWriteStream(const std::string& blobStoreDirectory, EncryptionAlgorithm, slice encryptionKey);

        ~BlobWriteStream() override;

        void write(slice) override;
        void close() override;

        [[nodiscard]] uint64_t bytesWritten() const { return _bytesWritten; }

        /** Derives the blobKey from the digest of the file data.
            No more data can be written after this is called. */
        C4BlobKey computeKey() noexcept;

        /** Moves the temporary file to the given path,
            or if a file already exists there, just deletes the temporary (since the existing
            file must have the same contents.) */
        void install(const FilePath& dstPath);

      private:
        bool deleteTempFile();

        FilePath                 _tmpPath;
        shared_ptr<WriteStream>  _writer;
        uint64_t                 _bytesWritten{0};
        SHA1Builder              _sha1ctx;
        std::optional<C4BlobKey> _blobKey;
        bool                     _installed{false};
    };

}  // namespace litecore
