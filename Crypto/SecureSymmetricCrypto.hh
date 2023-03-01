//
// SecureSymmetricCrypto.h
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"

namespace litecore {

    static const size_t kAES256KeySize = kEncryptionKeySize[kAES256];  // 256 bits
    static const size_t kAESBlockSize  = 16;                           // 128 bits (regardless of key size)
    static const size_t kAESIVSize     = kAESBlockSize;

    /** AES256 encryption/decryption. */
    size_t AES256(bool                  encrypt,  // true=encrypt, false=decrypt
                  slice                 key,      // pointer to 32-byte key
                  slice                 iv,       // pointer to 16-byte initialization vector
                  bool                  padding,  // true=PKCS7 padding, false=no padding
                  fleece::mutable_slice dst,      // output buffer & capacity
                  slice                 src);                     // input data

    /** Converts a password string into a key using PBKDF2. */
    bool DeriveKeyFromPassword(slice password, void* outKey, size_t keyLength);

    /** Converts a password string into a key using PBKDF2 and SHA1 as the hashing function. */
    bool DeriveKeyFromPasswordSHA1(slice password, void* outKey, size_t keyLength);
}  // namespace litecore
