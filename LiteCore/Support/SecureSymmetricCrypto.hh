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

#else

    #define AES256_AVAILABLE 0

#endif

}
