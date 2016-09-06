//
//  EncryptedStream.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/2/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "EncryptedStream.hh"
#include "Error.hh"
#include "LogInternal.hh"
#include "SecureRandomize.hh"
#include "SecureSymmetricCrypto.hh"
#include "forestdb_endian.h"


static const size_t kCipherBufSize = 32768;


namespace litecore {
    using namespace std;


    void EncryptedStream::initEncryptor(EncryptionAlgorithm alg,
                                        slice encryptionKey,
                                        slice nonce)
    {
        if (alg != kAES256)
            error::_throw(error::UnsupportedEncryption);
        memcpy(&_key, encryptionKey.buf, encryptionKey.size);
    }


    EncryptedStream::~EncryptedStream() {
    }


#pragma mark - WRITER:


    EncryptedWriteStream::EncryptedWriteStream(WriteStream *output,
                                               EncryptionAlgorithm alg,
                                               slice encryptionKey)
    :_output(output)
    {
        // Derive a random nonce with which to scramble the key, and write it to the file:
        uint8_t buf[kAESKeySize];
        slice nonce(buf, sizeof(buf));
        SecureRandomize(nonce);
        _output->write(nonce);

        initEncryptor(alg, encryptionKey, nonce);
    }


    EncryptedWriteStream::~EncryptedWriteStream() {
        close();
    }


    void EncryptedWriteStream::writeBlock(slice plaintext, bool finalBlock) {
        CBFAssert(plaintext.size <= kFileBlockSize);
        CBFDebugAssert(plaintext.size <= kCipherBufSize);
        uint64_t iv[2] = {0, _endian_encode(_blockID++)};
        uint8_t cipherBuf[kFileBlockSize + kAESBlockSize];
        slice ciphertext(cipherBuf, sizeof(cipherBuf));
        ciphertext.size = AES256(true,
                                 slice(&_key, sizeof(_key)), slice(iv, sizeof(iv)),
                                 finalBlock,
                                 ciphertext,
                                 plaintext);
        _output->write(ciphertext);
        Log("WRITE #%2lu: %lu bytes, final=%d --> %lu bytes ciphertext",
            _blockID-1, plaintext.size, finalBlock, ciphertext.size);
    }


    void EncryptedWriteStream::write(slice plaintext) {
        // Fill the current partial block buffer:
        auto capacity = min((size_t)kFileBlockSize - _bufferPos, plaintext.size);
        memcpy(&_buffer[_bufferPos], plaintext.buf, capacity);
        _bufferPos += capacity;
        plaintext.moveStart(capacity);
        if (plaintext.size == 0)
            return; // done; didn't overflow buffer

        // Write the completed buffer:
        writeBlock(slice(_buffer, kFileBlockSize), false);

        // Write entire blocks:
        while (plaintext.size > kFileBlockSize)
            writeBlock(plaintext.read(kFileBlockSize), false);

        // Save remainder in the buffer. This might be an entire block, because we don't know if
        // that's the last block, and we need to write the last block specially (padded).
        memcpy(_buffer, plaintext.buf, plaintext.size);
        _bufferPos = plaintext.size;
    }


    void EncryptedWriteStream::close() {
        if (_output) {
            // Write the final (possibly partial) block with PKCS7 padding:
            writeBlock(slice(_buffer, _bufferPos), true);
            delete _output;
            _output = nullptr;
        }
    }


#pragma mark - READER:


    EncryptedReadStream::EncryptedReadStream(SeekableReadStream *input,
                                             EncryptionAlgorithm alg,
                                             slice encryptionKey)
    :_input(input),
     _inputLength(_input->getLength() - kFileSizeOverhead),
     _finalBlockID((_inputLength - 1) / kFileBlockSize)
    {
        // Read the random nonce from the file:
        uint8_t buf[kAESKeySize];
        if (_input->read(buf, sizeof(buf)) < sizeof(buf))
            error::_throw(error::CorruptData);

        initEncryptor(alg, encryptionKey, slice(buf, sizeof(buf)));
    }


    EncryptedReadStream::~EncryptedReadStream() {
        close();
    }


    void EncryptedReadStream::close() {
        if (_input ) {
            _input->close();
            _input = nullptr;
        }
    }


    // Reads & decrypts the next block from the file into `output`
    size_t EncryptedReadStream::readBlockFromFile(slice output) {
        uint8_t blockBuf[kFileBlockSize + kAESBlockSize];
        bool finalBlock = (_blockID == _finalBlockID);
        size_t readSize = kFileBlockSize;
        if (finalBlock)
            readSize += kAESBlockSize;
        size_t bytesRead = _input->read(blockBuf, readSize);

        uint64_t iv[2] = {0, _endian_encode(_blockID++)};
        size_t outputSize = AES256(false,
                      slice(_key, sizeof(_key)),
                      slice(iv, sizeof(iv)),
                      finalBlock,
                      output, slice(blockBuf, bytesRead));
        Log("READ  #%2lu: %lu bytes, final=%d --> %lu bytes ciphertext",
            _blockID-1, bytesRead, finalBlock, outputSize);
        return outputSize;
    }


    // Reads the next block from the file into _buffer
    void EncryptedReadStream::fillBuffer() {
        _bufferBlockID = _blockID;
        _bufferSize = readBlockFromFile(slice(_buffer, kFileBlockSize));
        _bufferPos = 0;
    }


    // Reads as many bytes as possible from _buffer into `remaining`.
    void EncryptedReadStream::readFromBuffer(slice &remaining) {
        size_t nFromBuffer = min(_bufferSize - _bufferPos, remaining.size);
        if (nFromBuffer > 0) {
            remaining.writeFrom(slice(&_buffer[_bufferPos], nFromBuffer));
            _bufferPos += nFromBuffer;
        }
    }


    size_t EncryptedReadStream::read(void *dst, size_t count) {
        slice remaining(dst, count);
        // If there's decrypted data in the buffer, copy it to the output:
        readFromBuffer(remaining);
        if (remaining.size > 0 && _blockID <= _finalBlockID) {
            // Read & decrypt as many blocks as possible from the file to the output:
            while (remaining.size >= kFileBlockSize && _blockID <= _finalBlockID) {
                remaining.moveStart(readBlockFromFile(remaining));
            }

            if (remaining.size > 0) {
                // Partial block: decrypt entire block to buffer, then copy part to the output:
                fillBuffer();
                readFromBuffer(remaining);
            }
        }
        return (uint8_t*)remaining.buf - (uint8_t*)dst;
    }


    bool EncryptedReadStream::atEOF() const {
        return _blockID > _finalBlockID && _bufferPos == _bufferSize;
    }


    uint64_t EncryptedReadStream::getLength() const {
        if (_cleartextLength == UINT64_MAX)
            (const_cast<EncryptedReadStream*>(this))->findLength();
        return _cleartextLength;
    }

    void EncryptedReadStream::findLength() {
        // Have to read the final block of the file to determine its actual length:
        uint64_t pos = tell();
        seek(_inputLength);
        _cleartextLength = tell();
        seek(pos);
    }

    
    void EncryptedReadStream::seek(uint64_t pos) {
        //TODO: Optimize for case where pos is within the current _buffer
        if (pos > _inputLength)
            pos = _inputLength;
        uint64_t blockID = pos / kFileBlockSize;
        uint64_t blockPos = blockID * kFileBlockSize;
        if (blockID != _bufferBlockID) {
            Log("SEEK %lu (block %lu + %lu bytes)", pos, blockID, pos - blockPos);
            _input->seek(kFileSizeOverhead + blockPos);
            _blockID = blockID;
            fillBuffer();
        }
        _bufferPos = min((size_t)(pos - blockPos), _bufferSize);
    }


    uint64_t EncryptedReadStream::tell() const {
        if (_bufferBlockID == UINT64_MAX)
            return 0;
        return _bufferBlockID * kFileBlockSize + _bufferPos;
    }
    
}
