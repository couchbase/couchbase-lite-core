//
//  EncryptedStream.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/2/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Stream.hh"


namespace litecore {

    /** Abstract base class of EncryptedReadStream and EncryptedWriteStream. */
    class EncryptedStream {
    public:
        static const unsigned kFileSizeOverhead = 32;
        static const unsigned kFileBlockSize = 4096;

    protected:
        EncryptedStream() { }
        void initEncryptor(EncryptionAlgorithm alg,
                           slice encryptionKey,
                           slice nonce);
        virtual ~EncryptedStream();

        void checkErr() const;

        uint8_t _key[32];
        uint8_t _buffer[kFileBlockSize];    // stores partially read/written blocks across calls
        size_t _bufferPos {0};        // Indicates how much of buffer is used
        uint64_t _blockID   {0};        // Next block ID to be encrypted/decrypted (counter)
    };


    /** Encrypts data written to it, and writes it to a wrapped WriteStream. */
    class EncryptedWriteStream : public virtual EncryptedStream, public virtual WriteStream {
    public:
        EncryptedWriteStream(WriteStream *output,
                             EncryptionAlgorithm alg,
                             slice encryptionKey);
        ~EncryptedWriteStream();

        void write(slice) override;
        void close() override;

    private:
        void writeBlock(slice plaintext, bool finalBlock);

        WriteStream* _output;           // Wrapped stream that will write the ciphertext
    };


    /** Wraps a SeekableReadStream and decrypts its contents. */
    class EncryptedReadStream : public EncryptedStream, public virtual SeekableReadStream {
    public:
        EncryptedReadStream(SeekableReadStream *input,
                            EncryptionAlgorithm alg,
                            slice encryptionKey);
        ~EncryptedReadStream();
        bool atEOF() const override;
        uint64_t getLength() const override;
        size_t read(void *dst, size_t count) override;
        void seek(uint64_t pos) override;
        void close() override;
        uint64_t tell() const;

    private:
        size_t readBlockFromFile(slice output);
        void readFromBuffer(slice &dst);
        void fillBuffer();
        void findLength();

        SeekableReadStream* _input;       // Wrapped stream that ciphertext is read from
        uint64_t _inputLength;
        uint64_t _cleartextLength {UINT64_MAX};
        uint64_t _bufferBlockID {UINT64_MAX};
        uint64_t _finalBlockID;
        size_t _bufferSize {0};
    };
    
}
