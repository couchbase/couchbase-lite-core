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

#pragma once
#include "Base.hh"

namespace litecore {

    static const size_t kAES128KeySize = kEncryptionKeySize[kAES128]; // 128 bits
    static const size_t kAES256KeySize = kEncryptionKeySize[kAES256]; // 256 bits
    static const size_t kAESBlockSize  = 16; // 128 bits (regardless of key size)
    static const size_t kAESIVSize = kAESBlockSize;

// AES128() and AES256() may not be available on all platforms.
// Callers can use "#if AES256_AVAILABLE" to conditionalize code based on availability.

#if defined(_CRYPTO_CC) || defined(_CRYPTO_MBEDTLS)

    #define AES128_AVAILABLE 1

    size_t AES128(bool encrypt,        // true=encrypt, false=decrypt
                  slice key,           // pointer to 16-byte key
                  slice iv,            // pointer to 16-byte initialization vector
                  bool padding,        // true=PKCS7 padding, false=no padding
                  slice dst,           // output buffer & capacity
                  slice src);          // input data

#else
#define AES128_AVAILABLE 0
#endif


#if defined(_CRYPTO_CC) || defined(_CRYPTO_MBEDTLS)

    #define AES256_AVAILABLE 1

    size_t AES256(bool encrypt,        // true=encrypt, false=decrypt
                  slice key,           // pointer to 32-byte key
                  slice iv,            // pointer to 16-byte initialization vector
                  bool padding,        // true=PKCS7 padding, false=no padding
                  slice dst,           // output buffer & capacity
                  slice src);          // input data

    // TODO: Combine these into a single Encrypt() function that takes an algorithm parameter.

#else
#define AES256_AVAILABLE 0
#endif

}
