//
// BlobStreams.hh
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
#include "c4BlobStore.h"
#include "c4DatabaseTypes.h"
#include "FilePath.hh"
#include "SecureDigest.hh"
#include "Stream.hh"
#include <memory>
#include <optional>
#include <string>

namespace litecore {

    /** Returns a stream for reading a blob from the given file in the BlobStore. */
    unique_ptr<SeekableReadStream> OpenBlobReadStream(const FilePath &blobFile,
                                                      EncryptionAlgorithm,
                                                      slice encryptionKey);

    /** A stream for writing a new blob. */
    class BlobWriteStream final : public WriteStream {
    public:
        BlobWriteStream(const std::string &blobStoreDirectory,
                        EncryptionAlgorithm,
                        slice encryptionKey);

        ~BlobWriteStream();

        void write(slice) override;
        void close() override;

        uint64_t bytesWritten() const {
            return _bytesWritten;
        }

        /** Derives the blobKey from the digest of the file data.
            No more data can be written after this is called. */
        C4BlobKey computeKey() noexcept;

        /** Moves the temporary file to the given path,
            or if a file already exists there, just deletes the temporary (since the existing
            file must have the same contents.) */
        void install(const FilePath &dstPath);

    private:
        bool deleteTempFile();
        
        FilePath _tmpPath;
        shared_ptr<WriteStream> _writer;
        uint64_t _bytesWritten {0};
        SHA1Builder _sha1ctx;
        std::optional<C4BlobKey> _blobKey;
        bool _installed {false};
    };

}
