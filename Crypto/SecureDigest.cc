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
#    define USE_COMMON_CRYPTO
#endif

#ifdef USE_COMMON_CRYPTO
#    include <CommonCrypto/CommonDigest.h>
#    define CCONTEXT ((CC_SHA1_CTX*)_context)
#else
#    define CCONTEXT ((mbedtls_sha1_context*)_context)
#endif

namespace litecore {

    void SHA1::computeFrom(fleece::slice s) { (SHA1Builder() << s).finish(&_bytes, size()); }

    void SHA256::computeFrom(fleece::slice s) { (SHA256Builder() << s).finish(&_bytes, size()); }

    template <unsigned int N>
    bool Hash<N>::setDigest(fleece::slice s) {
        if ( s.size != size() ) return false;
        memcpy(_bytes, s.buf, size());
        return true;
    }

    template <unsigned int N>
    std::string Hash<N>::asBase64() const {
        return fleece::base64::encode(asSlice());
    }

    SHA1Builder::SHA1Builder() {
        static_assert(sizeof(_context) >= sizeof(mbedtls_sha1_context));
#ifdef USE_COMMON_CRYPTO
        static_assert(sizeof(_context) >= sizeof(CC_SHA1_CTX));
        CC_SHA1_Init(CCONTEXT);
#else
        mbedtls_sha1_init(CCONTEXT);
        mbedtls_sha1_starts(CCONTEXT);
#endif
    }

    SHA1Builder& SHA1Builder::operator<<(fleece::slice s) {
#ifdef USE_COMMON_CRYPTO
        CC_SHA1_Update(CCONTEXT, s.buf, (CC_LONG)s.size);
#else
        mbedtls_sha1_update(CCONTEXT, (unsigned char*)s.buf, s.size);
#endif
        return *this;
    }

    void SHA1Builder::finish(void* result, size_t resultSize) {
        DebugAssert(resultSize == sizeof(SHA1::_bytes));
#ifdef USE_COMMON_CRYPTO
        CC_SHA1_Final((uint8_t*)result, CCONTEXT);
#else
        mbedtls_sha1_finish(CCONTEXT, (uint8_t*)result);
        mbedtls_sha1_free(CCONTEXT);
#endif
    }

#undef CCONTEXT
#ifdef USE_COMMON_CRYPTO
#    include <CommonCrypto/CommonDigest.h>
#    define CCONTEXT ((CC_SHA256_CTX*)_context)
#else
#    define CCONTEXT ((mbedtls_sha256_context*)_context)
#endif

    SHA256Builder::SHA256Builder() {
        static_assert(sizeof(_context) >= sizeof(mbedtls_sha256_context));
#ifdef USE_COMMON_CRYPTO
        static_assert(sizeof(_context) >= sizeof(CC_SHA256_CTX));
        CC_SHA256_Init(CCONTEXT);
#else
        mbedtls_sha256_init(CCONTEXT);
        mbedtls_sha256_starts(CCONTEXT, 0);
#endif
    }

    SHA256Builder& SHA256Builder::operator<<(fleece::slice s) {
#ifdef USE_COMMON_CRYPTO
        CC_SHA256_Update(CCONTEXT, s.buf, (CC_LONG)s.size);
#else
        mbedtls_sha256_update(CCONTEXT, (unsigned char*)s.buf, s.size);
#endif
        return *this;
    }

    void SHA256Builder::finish(void* result, size_t resultSize) {
        DebugAssert(resultSize == sizeof(SHA256::_bytes));
#ifdef USE_COMMON_CRYPTO
        CC_SHA256_Final((uint8_t*)result, CCONTEXT);
#else
        mbedtls_sha256_finish(CCONTEXT, (uint8_t*)result);
        mbedtls_sha256_free(CCONTEXT);
#endif
    }

    // It seems odd to need these since they are base classes
    // But without them there are linker errors.
    template class Hash<20>;
    template class Hash<32>;

}  // namespace litecore
