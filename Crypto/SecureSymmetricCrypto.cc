//
// SecureSymmetricCrypto.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SecureSymmetricCrypto.hh"
#include "Error.hh"
#include "Logging.hh"

#ifdef __APPLE__
#    include <MacTypes.h>
#    include <CommonCrypto/CommonCrypto.h>
#else
#    include "mbedtls/cipher.h"
#    include "mbedtls/pkcs5.h"
#endif


namespace litecore {
    using namespace fleece;

    // Parameters for PBDKF2 key derivation:
    static constexpr slice kPBKDFSalt   = "Salty McNaCl"_sl;
    static constexpr int   kPBKDFRounds = 64000;

#ifdef __APPLE__

    // iOS and Mac OS implementation based on system-level CommonCrypto library:

    size_t AES256(bool encrypt, slice key, slice iv, bool padding, mutable_slice dst, slice src) {
        DebugAssert(key.size == kCCKeySizeAES256);
        DebugAssert(iv.buf == nullptr || iv.size == kCCBlockSizeAES128, "IV is wrong size");
        size_t          outSize;
        CCCryptorStatus status
                = CCCrypt((encrypt ? kCCEncrypt : kCCDecrypt), kCCAlgorithmAES, (padding ? kCCOptionPKCS7Padding : 0),
                          key.buf, key.size, iv.buf, src.buf, src.size, dst.buf, dst.size, &outSize);
        if ( status != kCCSuccess ) {
            Assert(status != kCCParamError && status != kCCBufferTooSmall && status != kCCUnimplemented);
            error::_throw(error::CryptoError);
        }
        return outSize;
    }

    bool DeriveKeyFromPassword(slice password, void* outKey, size_t keyLength) {
        int status = CCKeyDerivationPBKDF(kCCPBKDF2, (const char*)password.buf, password.size,
                                          (const uint8_t*)kPBKDFSalt.buf, kPBKDFSalt.size, kCCPRFHmacAlgSHA256,
                                          kPBKDFRounds, (uint8_t*)outKey, keyLength);
        return (status == noErr);
    }

    bool DeriveKeyFromPasswordSHA1(slice password, void* outKey, size_t keyLength) {
        int status = CCKeyDerivationPBKDF(kCCPBKDF2, (const char*)password.buf, password.size,
                                          (const uint8_t*)kPBKDFSalt.buf, kPBKDFSalt.size, kCCPRFHmacAlgSHA1,
                                          kPBKDFRounds, (uint8_t*)outKey, keyLength);
        return (status == noErr);
    }
#else

    // Cross-platform implementation using mbedTLS library:

    static size_t AES(size_t key_size, mbedtls_cipher_type_t cipher, bool encrypt, slice key, slice iv, bool padding,
                      mutable_slice dst, slice src) {
        DebugAssert(key.size == key_size);
        DebugAssert(iv.buf == nullptr || iv.size == kAESBlockSize, "IV is wrong size");
        mbedtls_cipher_context_t     cipher_ctx;
        const mbedtls_cipher_info_t* cipher_info = mbedtls_cipher_info_from_type(cipher);
        if ( cipher_info == nullptr ) {
            Warn("mbedtls_cipher_info_from_type failed");
            error::_throw(error::CryptoError);
        }

        mbedtls_cipher_init(&cipher_ctx);
        mbedtls_cipher_setup(&cipher_ctx, cipher_info);
        if ( padding ) {
            mbedtls_cipher_set_padding_mode(&cipher_ctx, MBEDTLS_PADDING_PKCS7);
        } else {
            mbedtls_cipher_set_padding_mode(&cipher_ctx, MBEDTLS_PADDING_NONE);
        }

        size_t out_len = dst.size;
        mbedtls_cipher_setkey(&cipher_ctx, (const unsigned char*)key.buf, (int)key_size * 8,
                              encrypt ? MBEDTLS_ENCRYPT : MBEDTLS_DECRYPT);
        mbedtls_cipher_crypt(&cipher_ctx, (const unsigned char*)iv.buf, iv.size, (const unsigned char*)src.buf,
                             src.size, (unsigned char*)dst.buf, &out_len);

        mbedtls_cipher_free(&cipher_ctx);
        return out_len;
    }

    size_t AES256(bool encrypt, slice key, slice iv, bool padding, mutable_slice dst, slice src) {
        return AES(kAES256KeySize, MBEDTLS_CIPHER_AES_256_CBC, encrypt, key, iv, padding, dst, src);
    }

    bool DeriveKeyFromPassword(slice password, void* outKey, size_t keyLength) {
        const mbedtls_md_info_t* digestType = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if ( !digestType ) return false;
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        int status = mbedtls_md_setup(&ctx, digestType, 1);
        if ( status != 0 ) return false;
        status = mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char*)password.buf, password.size,
                                           (const unsigned char*)kPBKDFSalt.buf, kPBKDFSalt.size, kPBKDFRounds,
                                           (int)keyLength, (unsigned char*)outKey);
        mbedtls_md_free(&ctx);
        return (status == 0);
    }

    bool DeriveKeyFromPasswordSHA1(slice password, void* outKey, size_t keyLength) {
        const mbedtls_md_info_t* digestType = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
        if ( !digestType ) return false;
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        int status = mbedtls_md_setup(&ctx, digestType, 1);
        if ( status != 0 ) return false;
        status = mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char*)password.buf, password.size,
                                           (const unsigned char*)kPBKDFSalt.buf, kPBKDFSalt.size, kPBKDFRounds,
                                           (int)keyLength, (unsigned char*)outKey);
        mbedtls_md_free(&ctx);
        return (status == 0);
    }

#endif

}  // namespace litecore
