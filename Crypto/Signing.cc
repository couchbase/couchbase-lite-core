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
#include "mbedUtils.hh"
#include "monocypher.h"
#include "monocypher-ed25519.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedtls/pk.h"
#pragma clang diagnostic pop

namespace litecore::crypto {
    using namespace std;


    std::unique_ptr<SigningKey> SigningKey::generate(const char *algorithm) {
        if (0 == strcmp(algorithm, kRSAAlgorithmName)) {
            return make_unique<RSASigningKey>(PrivateKey::generateTemporaryRSA(2048));
        } else if (0 == strcmp(algorithm, kEd25519AlgorithmName)) {
            return make_unique<Ed25519SigningKey>();
        } else {
            error::_throw(error::CryptoError, "Unknown signature algorithm '%s'", algorithm);
        }
    }


    unique_ptr<VerifyingKey> VerifyingKey::instantiate(slice data, const char *algorithm) {
        if (0 == strcmp(algorithm, kRSAAlgorithmName)) {
            return make_unique<RSAVerifyingKey>(data);
        } else if (0 == strcmp(algorithm, kEd25519AlgorithmName)) {
            return make_unique<Ed25519VerifyingKey>(data);
        } else {
            error::_throw(error::CryptoError, "Unknown signature algorithm '%s'", algorithm);
        }
    }


#pragma mark - RSA:


    static int rngFunction(void *ctx, unsigned char *dst, size_t size) {
        SecureRandomize({dst, size});
        return 0;
    }


    alloc_slice RSASigningKey::sign(slice data) const {
        SHA256 inputDigest(data);
        alloc_slice signature(MBEDTLS_PK_SIGNATURE_MAX_SIZE);
        size_t sigLen = 0;
        TRY(mbedtls_pk_sign(_key->context(),
                            MBEDTLS_MD_SHA256,  // declares that input is a SHA256 digest.
                            (const uint8_t*)inputDigest.asSlice().buf, inputDigest.asSlice().size,
                            (uint8_t*)signature.buf, &sigLen,
                            rngFunction, nullptr));
        signature.shorten(sigLen);
        return signature;
    }

    
    unique_ptr<VerifyingKey> RSASigningKey::verifyingKey() const {
        return make_unique<RSAVerifyingKey>(_key->publicKey());
    }


    bool RSAVerifyingKey::verifySignature(slice data, slice signature) const {
        SHA256 inputDigest(data);
        int result = mbedtls_pk_verify(_key->context(),
                                       MBEDTLS_MD_SHA256, // declares that input is a SHA256 digest.
                                       (const uint8_t*)inputDigest.asSlice().buf,
                                       inputDigest.asSlice().size,
                                       (const uint8_t*)signature.buf, signature.size);
        if (result == MBEDTLS_ERR_RSA_VERIFY_FAILED)
            return false;
        TRY(result);        // other error codes throw exceptions
        return true;
    }


#pragma mark - Ed25519:


    Ed25519Base::Ed25519Base(slice bytes) {
        if (bytes.size != sizeof(_bytes))
            error::_throw(error::CryptoError, "Invalid data size for Ed25519 key");
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


    unique_ptr<VerifyingKey> Ed25519SigningKey::verifyingKey() const {
        unique_ptr<Ed25519VerifyingKey> pub(new Ed25519VerifyingKey());
        crypto_ed25519_public_key(pub->_bytes.data(), _bytes.data());
        return pub;
    }


    alloc_slice Ed25519SigningKey::verifyingKeyData() const {
        return publicKey().data();
    }


    alloc_slice Ed25519SigningKey::sign(slice data) const {
        alloc_slice signature(kSignatureSize);
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
        return signature.size == kSignatureSize
            && 0 == crypto_ed25519_check((const uint8_t*)signature.buf,
                                         (const uint8_t*)_bytes.data(),
                                         (const uint8_t*)inputData.buf,
                                         inputData.size);
    }

}
