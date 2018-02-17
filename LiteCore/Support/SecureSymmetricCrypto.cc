//
// SecureSymmetricCrypto.cc
//
// Copyright (c) 2016-2018 Couchbase, Inc All rights reserved.
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

#include "SecureSymmetricCrypto.hh"
#include "Error.hh"

#if defined(_CRYPTO_CC)
    #include <CommonCrypto/CommonCryptor.h>
#elif defined(_CRYPTO_OPENSSL)
    #include <openssl/conf.h>
    #include <openssl/evp.h>
    #include <openssl/err.h>
    #include <string.h>
#elif defined(_CRYPTO_MBEDTLS)
    #include <mbedtls/cipher.h>
#endif


namespace litecore {

#if defined(_CRYPTO_CC)

    // iOS and Mac OS implementation based on system-level CommonCrypto library:

    size_t AES128(bool encrypt,
                  slice key,
                  slice iv,
                  bool padding,
                  slice dst,
                  slice src)
    {
        DebugAssert(key.size == kCCKeySizeAES128);
        DebugAssert(iv.buf == nullptr || iv.size == kCCBlockSizeAES128, "IV is wrong size");
        size_t outSize;
        CCCryptorStatus status = CCCrypt((encrypt ? kCCEncrypt : kCCDecrypt),
                                         kCCAlgorithmAES,
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


    size_t AES256(bool encrypt,
                  slice key,
                  slice iv,
                  bool padding,
                  slice dst,
                  slice src)
    {
        DebugAssert(key.size == kCCKeySizeAES256);
        DebugAssert(iv.buf == nullptr || iv.size == kCCBlockSizeAES128, "IV is wrong size");
        size_t outSize;
        CCCryptorStatus status = CCCrypt((encrypt ? kCCEncrypt : kCCDecrypt),
                                         kCCAlgorithmAES,
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


#elif defined(_CRYPTO_OPENSSL)

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

    size_t AES256(bool encrypt,
                  slice key,
                  slice iv,
                  bool padding,
                  slice dst,
                  slice src)
    {
        DebugAssert(key.size == kAES256KeySize);
        DebugAssert(iv.buf == nullptr || iv.size == kAESBlockSize, "IV is wrong size");

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


#elif defined(_CRYPTO_MBEDTLS)

    size_t AES256(bool encrypt,
                  slice key,
                  slice iv,
                  bool padding,
                  slice dst,
                  slice src)
    {
        DebugAssert(key.size == kAES256KeySize);
        DebugAssert(iv.buf == nullptr || iv.size == kAESBlockSize, "IV is wrong size");
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

#endif

}

