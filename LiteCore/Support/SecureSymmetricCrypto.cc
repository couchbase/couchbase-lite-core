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
#include "Logging.hh"

#if defined(_CRYPTO_CC)
    #include <CommonCrypto/CommonCryptor.h>
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

#elif defined(_CRYPTO_MBEDTLS)

	size_t AES(size_t key_size,
			   mbedtls_cipher_type_t cipher,
		       bool encrypt,
               slice key,
               slice iv,
               bool padding,
               slice dst,
               slice src)
    {
	    DebugAssert(key.size == key_size);
        DebugAssert(iv.buf == nullptr || iv.size == kAESBlockSize, "IV is wrong size");
        mbedtls_cipher_context_t cipher_ctx;
        const mbedtls_cipher_info_t *cipher_info = mbedtls_cipher_info_from_type( cipher );
        if(cipher_info == nullptr) {
            Warn("mbedtls_cipher_info_from_type failed");
            error::_throw(error::CryptoError);
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
        mbedtls_cipher_setkey(&cipher_ctx, (const unsigned char*)key.buf, key_size * 8, encrypt ? MBEDTLS_ENCRYPT : MBEDTLS_DECRYPT);
        mbedtls_cipher_crypt(&cipher_ctx, (const unsigned char*)iv.buf, iv.size, (const unsigned char*)src.buf, src.size,
                             (unsigned char*)dst.buf, &out_len);

        mbedtls_cipher_free(&cipher_ctx);
        return out_len;
    }

	size_t AES128(bool encrypt,
                  slice key,
                  slice iv,
                  bool padding,
                  slice dst,
                  slice src)
    {
        return AES(kAES128KeySize, MBEDTLS_CIPHER_AES_128_CBC, encrypt, key, iv, padding, dst, src);
    }

    size_t AES256(bool encrypt,
                  slice key,
                  slice iv,
                  bool padding,
                  slice dst,
                  slice src)
    {
        return AES(kAES256KeySize, MBEDTLS_CIPHER_AES_256_CBC, encrypt, key, iv, padding, dst, src);
    }

#endif

}

