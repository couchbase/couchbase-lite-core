//
// SecureSymmetricCrypto.h
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
//
//  Based on crypto_primitives.h from ForestDB

#pragma once
#include "Base.hh"
#ifdef _CRYPTO_OPENSSL
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>
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
        DebugAssert(key.size == kCCKeySizeAES256);
        DebugAssert(iv.buf == nullptr || iv.size == kCCBlockSizeAES128, "IV is wrong size");
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
            Assert(status != kCCParamError && status != kCCBufferTooSmall &&
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
        DebugAssert(key.size == KEY_SIZE);
        DebugAssert(iv.buf == nullptr || iv.size == BLOCK_SIZE, "IV is wrong size");

        auto init = EVP_EncryptInit_ex;
        auto update = EVP_EncryptUpdate;
        auto final = EVP_EncryptFinal_ex;
        if (!encrypt) {
            init = EVP_DecryptInit_ex;
            update = EVP_DecryptUpdate;
            final = EVP_DecryptFinal_ex;
        }

        EVP_CIPHER_CTX_free_ptr ctx(EVP_CIPHER_CTX_new(), ::EVP_CIPHER_CTX_free);
        check(init(ctx.get(), EVP_aes_256_cbc(), nullptr, (const byte *)key.buf, (const byte *)iv.buf));

        if (!padding)
            EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

        int outSize;
        check(update(ctx.get(), (byte*)dst.buf, &outSize, (const byte*)src.buf, (int)src.size));
        
        int outSize2 = (int)dst.size - outSize;
        int finalResult = final(ctx.get(), (byte*)dst.buf + outSize, &outSize2);
        if (encrypt) {
            check(finalResult);
        } else if (finalResult <= 0) {
            throw error::CryptoError;
        }

        return outSize + outSize2;
    }

    #define AES256_AVAILABLE 1
    
#elif defined(_CRYPTO_MBEDTLS)

    static const unsigned int KEY_SIZE = 32;
    static const unsigned int BLOCK_SIZE = 16;
    
    #include <mbedtls/cipher.h>

    static size_t AES256(bool encrypt,      // true=encrypt, false=decrypt
        slice key,           // pointer to 32-byte key
        slice iv,            // pointer to 32-byte iv
        bool padding,        // true=PKCS7 padding, false=no padding
        slice dst,           // output buffer & capacity
        slice src)           // input data
    {
        DebugAssert(key.size == KEY_SIZE);
        DebugAssert(iv.buf == nullptr || iv.size == BLOCK_SIZE, "IV is wrong size");
        mbedtls_cipher_context_t cipher_ctx;
        const mbedtls_cipher_info_t *cipher_info = mbedtls_cipher_info_from_type( MBEDTLS_CIPHER_AES_256_CBC );
        if(cipher_info == NULL) {
            Warn("mbedtls_cipher_info_from_type failed");
            litecore::error::_throw(litecore::error::CryptoError);
        }
        
        mbedtls_cipher_init(&cipher_ctx);
        mbedtls_cipher_setup(&cipher_ctx, cipher_info);
        if (padding) {
            mbedtls_cipher_set_padding_mode(&cipher_ctx, MBEDTLS_PADDING_PKCS7);
        }
        else {
            mbedtls_cipher_set_padding_mode(&cipher_ctx, MBEDTLS_PADDING_NONE);
        }
        
        size_t out_len = dst.size;
        mbedtls_cipher_setkey(&cipher_ctx, (const unsigned char*)key.buf, 256, encrypt ? MBEDTLS_ENCRYPT : MBEDTLS_DECRYPT);
        mbedtls_cipher_crypt(&cipher_ctx, (const unsigned char*)iv.buf, iv.size, (const unsigned char*)src.buf, src.size,
                             (unsigned char*)dst.buf, &out_len);

        mbedtls_cipher_free(&cipher_ctx);
        return out_len;
    }
    
    #define AES256_AVAILABLE 1


#else

    #define AES256_AVAILABLE 0

#endif

}
