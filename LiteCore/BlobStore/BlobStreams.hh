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
#pragma once
#include "c4DatabaseTypes.h"
#include "FilePath.hh"
#include "SecureDigest.hh"
#include "Stream.hh"
#include <memory>
#include <string>

namespace litecore {

    /** A raw SHA-1 digest used as the unique identifier of a blob. Equivalent to C4BlobKey. */
    struct blobKey {
        SHA1 digest;

        blobKey() =default;
        blobKey(slice rawBytes);

        static blobKey withBase64(slice base64, bool prefixed =true);

        bool readFromBase64(slice base64, bool prefixed =true);
        bool readFromFilename(slice filename);

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


    /** A stream for writing a new Blob. */
    class BlobWriteStream final : public WriteStream {
    public:
        BlobWriteStream(const std::string &dir, EncryptionAlgorithm alg, slice encryptionKey);
        ~BlobWriteStream();

        void write(slice) override;
        void close() override;

        uint64_t bytesWritten() const                       {return _bytesWritten;}

        /** Derives the blobKey from the digest of the file data.
            No more data can be written after this is called. */
        blobKey computeKey() noexcept;

        /** Moves the temporary file to the given path.
            If a file already exists there, it just deletes the temporary. */
        void install(const FilePath &dstPath);

    private:
        FilePath _tmpPath;
        shared_ptr<WriteStream> _writer;
        uint64_t _bytesWritten {0};
        SHA1Builder _sha1ctx;
        blobKey _key;
        bool _computedKey {false};
        bool _installed {false};
    };

}
