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

    static const size_t kAES256KeySize = kEncryptionKeySize[kAES256]; // 256 bits
    static const size_t kAESBlockSize  = 16; // 128 bits (regardless of key size)
    static const size_t kAESIVSize = kAESBlockSize;

    /** AES256 encryption/decryption. */
    size_t AES256(bool encrypt,        // true=encrypt, false=decrypt
                  slice key,           // pointer to 32-byte key
                  slice iv,            // pointer to 16-byte initialization vector
                  bool padding,        // true=PKCS7 padding, false=no padding
                  fleece::mutable_slice dst,   // output buffer & capacity
                  slice src);          // input data

    /** Converts a password string into a key using PBKDF2. */
    bool DeriveKeyFromPassword(slice password,
                               void *outKey,
                               size_t keyLength);

}
