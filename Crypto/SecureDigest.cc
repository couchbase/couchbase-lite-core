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
#pragma clang diagnostic pop

#ifdef __APPLE__
#define USE_COMMON_CRYPTO
#endif

#ifdef USE_COMMON_CRYPTO
    #include <CommonCrypto/CommonDigest.h>
    #define _CONTEXT ((CC_SHA1_CTX*)_context)
#else
    #define _CONTEXT ((mbedtls_sha1_context*)_context)
#endif

namespace litecore {

    void SHA1::computeFrom(fleece::slice s) {
        (SHA1Builder() << s).finish(&bytes, sizeof(bytes));
    }


    bool SHA1::setDigest(fleece::slice s) {
        if (s.size != sizeof(bytes))
            return false;
        memcpy(bytes, s.buf, sizeof(bytes));
        return true;
    }


    std::string SHA1::asBase64() const {
        return fleece::base64::encode(asSlice());
    }


    SHA1Builder::SHA1Builder() {
        static_assert(sizeof(_context) >= sizeof(mbedtls_sha1_context));
#ifdef USE_COMMON_CRYPTO
        static_assert(sizeof(_context) >= sizeof(CC_SHA1_CTX));
        CC_SHA1_Init(_CONTEXT);
#else
        mbedtls_sha1_init(_CONTEXT);
        mbedtls_sha1_starts(_CONTEXT);
#endif
    }


    SHA1Builder& SHA1Builder::operator<< (fleece::slice s) {
#ifdef USE_COMMON_CRYPTO
        CC_SHA1_Update(_CONTEXT, s.buf, (CC_LONG)s.size);
#else
        mbedtls_sha1_update(_CONTEXT, (unsigned char*)s.buf, s.size);
#endif
        return *this;
    }


    void SHA1Builder::finish(void *result, size_t resultSize) {
        DebugAssert(resultSize == sizeof(SHA1::bytes));
#ifdef USE_COMMON_CRYPTO
        CC_SHA1_Final((uint8_t*)result, _CONTEXT);
#else
        mbedtls_sha1_finish(_CONTEXT, (uint8_t*)result);
        mbedtls_sha1_free(_CONTEXT);
#endif
    }

}
