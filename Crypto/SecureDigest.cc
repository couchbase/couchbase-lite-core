//
// SecureDigest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SecureDigest.hh"
#include "Base64.hh"
#include "Error.hh"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#pragma clang diagnostic pop

#ifdef __APPLE__
#  define USE_COMMON_CRYPTO
#endif

#ifdef USE_COMMON_CRYPTO
    #include <CommonCrypto/CommonDigest.h>
#endif

namespace litecore {


    template <DigestType TYPE, size_t SIZE>
    bool Digest<TYPE,SIZE>::setDigest(fleece::slice s) {
        if (s.size != _bytes.size())
            return false;
        s.copyTo(_bytes.data());
        return true;
    }


    template <DigestType TYPE, size_t SIZE>
    std::string Digest<TYPE,SIZE>::asBase64() const {
        return fleece::base64::encode(asSlice());
    }


#pragma mark - SHA1:


    template <>
    Digest<SHA,1>::Builder::Builder() {
        static_assert(sizeof(_context) >= sizeof(mbedtls_sha1_context));
#ifdef USE_COMMON_CRYPTO
        static_assert(sizeof(_context) >= sizeof(CC_SHA1_CTX));
        CC_SHA1_Init((CC_SHA1_CTX*)_context);
#else
        mbedtls_sha1_init((mbedtls_sha1_context*)_context);
        mbedtls_sha1_starts((mbedtls_sha1_context*)_context);
#endif
    }


    template <>
    Digest<SHA,1>::Builder& Digest<SHA,1>::Builder::operator<< (fleece::slice s) {
#ifdef USE_COMMON_CRYPTO
        CC_SHA1_Update((CC_SHA1_CTX*)_context, s.buf, (CC_LONG)s.size);
#else
        mbedtls_sha1_update((mbedtls_sha1_context*)_context, (unsigned char*)s.buf, s.size);
#endif
        return *this;
    }


    template <>
    void Digest<SHA,1>::Builder::finish(void *result, size_t resultSize) {
        Assert(resultSize == kSizeInBytes);
#ifdef USE_COMMON_CRYPTO
        CC_SHA1_Final((uint8_t*)result, (CC_SHA1_CTX*)_context);
#else
        mbedtls_sha1_finish((mbedtls_sha1_context*)_context, (uint8_t*)result);
        mbedtls_sha1_free((mbedtls_sha1_context*)_context);
#endif
    }

    // Force the non-specialized methods to be instantiated:
    template class Digest<SHA,1>;


#pragma mark - SHA256:


    template <>
    Digest<SHA,256>::Builder::Builder() {
        static_assert(sizeof(_context) >= sizeof(mbedtls_sha256_context));
#ifdef USE_COMMON_CRYPTO
        static_assert(sizeof(_context) >= sizeof(CC_SHA256_CTX));
        CC_SHA256_Init((CC_SHA256_CTX*)_context);
#else
        mbedtls_sha256_init((mbedtls_sha256_context*)_context);
        mbedtls_sha256_starts((mbedtls_sha256_context*)_context, 0);
#endif
    }


    template <>
    Digest<SHA,256>::Builder& Digest<SHA,256>::Builder::operator<< (fleece::slice s) {
#ifdef USE_COMMON_CRYPTO
        CC_SHA256_Update((CC_SHA256_CTX*)_context, s.buf, (CC_LONG)s.size);
#else
        mbedtls_sha256_update((mbedtls_sha256_context*)_context, (unsigned char*)s.buf, s.size);
#endif
        return *this;
    }


    template <>
    void Digest<SHA,256>::Builder::finish(void *result, size_t resultSize) {
        Assert(resultSize == kSizeInBytes);
#ifdef USE_COMMON_CRYPTO
        CC_SHA256_Final((uint8_t*)result, (CC_SHA256_CTX*)_context);
#else
        mbedtls_sha256_finish((mbedtls_sha256_context*)_context, (uint8_t*)result);
        mbedtls_sha256_free((mbedtls_sha256_context*)_context);
#endif
    }


    // Force the non-specialized methods to be instantiated:
    template class Digest<SHA,256>;

}
