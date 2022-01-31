//
// Signing.cc
//
// Copyright © 2022 Couchbase. All rights reserved.
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

#include "Signing.hh"
#include "Error.hh"
#include "SecureRandomize.hh"
#include "monocypher.h"
#include "monocypher-ed25519.h"

namespace litecore::crypto {
    using namespace std;


    unique_ptr<VerifyingKey> VerifyingKey::instantiate(slice data, const char *algorithm) {
        if (0 == strcmp(algorithm, kRSAAlgorithmName)) {
            return make_unique<RSAVerifyingKey>(data);
        } else if (0 == strcmp(algorithm, kEd25519AlgorithmName)) {
            return make_unique<Ed25519VerifyingKey>(data);
        } else {
            error::_throw(error::CryptoError, "Unknown signature algorithm '%s'", algorithm);
        }
    }


    Ed25519Base::Ed25519Base(slice bytes) {
        if (bytes.size != sizeof(_bytes))
            error::_throw(error::CryptoError, "Invalid data for Ed25519 key");
        bytes.copyTo(_bytes.data());
    }


    Ed25519SigningKey::Ed25519SigningKey() {
        SecureRandomize({_bytes.data(), _bytes.size()});
    }


    Ed25519VerifyingKey Ed25519SigningKey::publicKey() const{
        Ed25519VerifyingKey pub;
        crypto_ed25519_public_key(pub._bytes.data(), _bytes.data());
        return pub;
    }


    alloc_slice Ed25519SigningKey::verifyingKeyData() const {
        return publicKey().data();
    }


    alloc_slice Ed25519SigningKey::sign(slice data) const {
        alloc_slice signature(64);
        crypto_ed25519_sign((uint8_t*)signature.buf,
                            (const uint8_t*)_bytes.data(), nullptr,
                            (const uint8_t*)data.buf, data.size);
        return signature;
    }


    Ed25519VerifyingKey::Ed25519VerifyingKey(slice bytes) {
        Assert(bytes.size == sizeof(_bytes));
        bytes.copyTo(_bytes.data());
    }

    bool Ed25519VerifyingKey::verifySignature(slice inputData, slice signature) const {
        return 0 == crypto_ed25519_check((const uint8_t*)signature.buf,
                                         (const uint8_t*)_bytes.data(),
                                         (const uint8_t*)inputData.buf,
                                         inputData.size);
    }

}
