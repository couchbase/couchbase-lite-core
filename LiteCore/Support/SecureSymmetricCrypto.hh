//
//  SecureSymmetricCrypto.h
//  LiteCore
//
//  Created by Jens Alfke on 9/2/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Based on crypto_primitives.h from ForestDB

#pragma once
#include "Base.hh"
#ifdef _CRYPTO_OPENSSL
#include <openssl\conf.h>
#include <openssl\evp.h>
#include <openssl\err.h>
#include <string.h>
#endif

namespace litecore {

    static const size_t kAESKeySize = 32; // 256 bits
    static const size_t kAESBlockSize = 16; // 128 bits
    static const size_t kAESIVSize = kAESBlockSize;

// Defines inline functions sha256 and aes256 if implementations are available.
// Callers can use #if SHA256_AVAILABLE" or "#if AES256_AVAILABLE" to conditionalize code based on
// availability.

#if defined(_CRYPTO_CC)

    // iOS and Mac OS implementation based on system-level CommonCrypto library:
    #include <CommonCrypto/CommonCryptor.h>
    #include <assert.h>



    static size_t AES256(bool encrypt,      // true=encrypt, false=decrypt
                       slice key,           // pointer to 32-byte key
                       slice iv,            // pointer to 32-byte iv
                       bool padding,        // true=PKCS7 padding, false=no padding
                       slice dst,           // output buffer & capacity
                       slice src)           // input data
    {
        CBFDebugAssert(key.size == kCCKeySizeAES256);
        CBFDebugAssert(iv.buf == nullptr || iv.size == kCCBlockSizeAES128);
        size_t outSize;
        CCCryptorStatus status = CCCrypt((encrypt ? kCCEncrypt : kCCDecrypt),
                                         kCCAlgorithmAES128,
                                         (padding ? kCCOptionPKCS7Padding : 0),
                                         key.buf, key.size,
                                         iv.buf,
                                         src.buf, src.size,
                                         (void*)dst.buf, dst.size,
                                         &outSize);
        if (status != kCCSuccess) {
            CBFAssert(status != kCCParamError && status != kCCBufferTooSmall &&
                      status != kCCUnimplemented);
            error::_throw(error::CryptoError);
        }
        return outSize;
    }

    #define AES256_AVAILABLE 1

#elif defined(_CRYPTO_OPENSSL)

    static const unsigned int KEY_SIZE = 32;
    static const unsigned int BLOCK_SIZE = 16;

    typedef unsigned char byte;
    using EVP_CIPHER_CTX_free_ptr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)>;

    static bool _IsSetup;
    static void _Init()
    {
        if (!_IsSetup) {
            EVP_add_cipher(EVP_aes_256_cbc());
        }

        _IsSetup = true;
    }

    static void check(int rc)
    {
        if (rc != 1) {
            error::_throw(error::CryptoError);
        }
    }

    static size_t AES256(bool encrypt,      // true=encrypt, false=decrypt
        slice key,           // pointer to 32-byte key
        slice iv,            // pointer to 32-byte iv
        bool padding,        // true=PKCS7 padding, false=no padding
        slice dst,           // output buffer & capacity
        slice src)           // input data
    {
        CBFDebugAssert(key.size == KEY_SIZE);
        CBFDebugAssert(iv.buf == nullptr || iv.size == BLOCK_SIZE);

        auto init = EVP_EncryptInit_ex;
        auto update = EVP_EncryptUpdate;
        auto final = EVP_EncryptFinal;
        if (!encrypt) {
            init = EVP_DecryptInit_ex;
            update = EVP_DecryptUpdate;
            final = EVP_EncryptFinal;
        }

        EVP_CIPHER_CTX_free_ptr ctx(EVP_CIPHER_CTX_new(), ::EVP_CIPHER_CTX_free);
        check(init(ctx.get(), EVP_aes_256_cbc(), NULL, (const byte *)key.buf, (const byte *)iv.buf));

        int outSize;
        check(update(ctx.get(), (byte*)dst.buf, &outSize, (const byte*)src.buf, (int)src.size));
        
        int outSize2 = (int)dst.size - outSize;
        check(final(ctx.get(), (byte*)dst.buf + outSize, &outSize2));

        return outSize + outSize2;
    }

    #define AES256_AVAILABLE 1

#else

    #define AES256_AVAILABLE 0

#endif

}
