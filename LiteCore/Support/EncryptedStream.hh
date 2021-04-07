//
// EncryptedStream.hh
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
#include "Stream.hh"
#include "fleece/slice_stream.hh"
#include <memory>

namespace litecore {

    /** Abstract base class of EncryptedReadStream and EncryptedWriteStream. */
    class EncryptedStream {
    public:
        static constexpr size_t kKeySize = kEncryptionKeySize[kAES256];
        static const unsigned kFileSizeOverhead = kKeySize;
        static const unsigned kFileBlockSize = 4096;

    protected:
        EncryptedStream() =default;
        void initEncryptor(EncryptionAlgorithm alg,
                           slice encryptionKey,
                           slice nonce);
        virtual ~EncryptedStream();

        EncryptionAlgorithm _alg;
        uint8_t _key[kKeySize];
        uint8_t _nonce[kKeySize];
        uint8_t _buffer[kFileBlockSize];    // stores partially read/written blocks across calls
        size_t _bufferPos {0};        // Indicates how much of buffer is used
        uint64_t _blockID   {0};        // Next block ID to be encrypted/decrypted (counter)
    };


    /** Encrypts data written to it, and writes it to a wrapped WriteStream. */
    class EncryptedWriteStream final : public virtual EncryptedStream, public virtual WriteStream {
    public:
        EncryptedWriteStream(std::shared_ptr<WriteStream> output,
                             EncryptionAlgorithm alg,
                             slice encryptionKey);
        ~EncryptedWriteStream();

        void write(slice) override;
        void close() override;

    private:
        void writeBlock(slice plaintext, bool finalBlock);

        std::shared_ptr<WriteStream> _output;    // Wrapped stream that will write the ciphertext
    };


    /** Provides (random) access to a data stream encrypted by EncryptedWriteStream. */
    class EncryptedReadStream final : public EncryptedStream, public virtual SeekableReadStream {
    public:
        EncryptedReadStream(std::shared_ptr<SeekableReadStream> input,
                            EncryptionAlgorithm alg,
                            slice encryptionKey);
        uint64_t getLength() const override;
        size_t read(void *dst NONNULL, size_t count) override;
        void seek(uint64_t pos) override;
        void close() override;
        uint64_t tell() const;

    private:
        size_t readBlockFromFile(fleece::mutable_slice);
        void readFromBuffer(fleece::slice_stream &dst);
        void fillBuffer();
        void findLength();

        std::shared_ptr<SeekableReadStream> _input;  // Wrapped stream that ciphertext is read from
        uint64_t _inputLength;
        uint64_t _cleartextLength {UINT64_MAX};
        uint64_t _bufferBlockID {UINT64_MAX};
        uint64_t _finalBlockID;
        size_t _bufferSize {0};
    };
    
}
