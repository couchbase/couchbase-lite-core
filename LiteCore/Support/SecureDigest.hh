//
// SecureDigest.hh
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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


#if defined(_CRYPTO_CC)

    // iOS and Mac OS implementation based on system-level CommonCrypto library:
    #include <CommonCrypto/CommonDigest.h>
    #include <assert.h>

    typedef CC_MD5_CTX md5Context;

    static inline void md5_begin(md5Context *ctx) {
        CC_MD5_Init(ctx);
    }
    static inline void md5_add(md5Context *ctx, const void *bytes, size_t length) {
        CC_MD5_Update(ctx, bytes, (CC_LONG)length);
    }
    static inline void md5_end(md5Context *ctx, void *outDigest) {
        CC_MD5_Final((uint8_t*)outDigest, ctx);
    }


    typedef CC_SHA1_CTX sha1Context;

    static inline void sha1_begin(sha1Context *ctx) {
        CC_SHA1_Init(ctx);
    }
    static inline void sha1_add(sha1Context *ctx, const void *bytes, size_t length) {
        CC_SHA1_Update(ctx, bytes, (CC_LONG)length);
    }
    static inline void sha1_end(sha1Context *ctx, void *outDigest) {
        CC_SHA1_Final((uint8_t*)outDigest, ctx);
    }


    typedef CC_SHA256_CTX sha256Context;

    static inline void sha256_begin(sha256Context *ctx) {
        CC_SHA256_Init(ctx);
    }
    static inline void sha256_add(sha256Context *ctx, const void *bytes, size_t length) {
        CC_SHA256_Update(ctx, bytes, (CC_LONG)length);
    }
    static inline void sha256_end(sha256Context *ctx, void *outDigest) {
        CC_SHA256_Final((uint8_t*)outDigest, ctx);
    }

    #define SECURE_DIGEST_AVAILABLE 1

#elif defined(_CRYPTO_LIBTOMCRYPT)

    #include <tomcrypt.h>
    // TODO
    #define SECURE_DIGEST_AVAILABLE 0

#elif defined(_CRYPTO_OPENSSL)

    #include <openssl/sha.h>
    #include <openssl/md5.h>

    typedef MD5_CTX md5Context;

    static inline void md5_begin(md5Context *ctx) {
        MD5_Init(ctx);
    }
    static inline void md5_add(md5Context *ctx, const void *bytes, size_t length) {
        MD5_Update(ctx, bytes, length);
    }
    static inline void md5_end(md5Context *ctx, void *outDigest) {
        MD5_Final((unsigned char*)outDigest, ctx);
    }


    typedef SHA_CTX sha1Context;

    static inline void sha1_begin(sha1Context *ctx) {
        SHA1_Init(ctx);
    }
    static inline void sha1_add(sha1Context *ctx, const void *bytes, size_t length) {
        SHA1_Update(ctx, bytes, length);
    }
    static inline void sha1_end(sha1Context *ctx, void *outDigest) {
        SHA1_Final((unsigned char *)outDigest, ctx);
    }


    typedef SHA256_CTX sha256Context;

    static inline void sha256_begin(sha256Context *ctx) {
        SHA256_Init(ctx);
    }
    static inline void sha256_add(sha256Context *ctx, const void *bytes, size_t length) {
        SHA256_Update(ctx, bytes, length);
    }
    static inline void sha256_end(sha256Context *ctx, void *outDigest) {
        SHA256_Final((unsigned char *)outDigest, ctx);
    }

    #define SECURE_DIGEST_AVAILABLE 1
    
#elif defined(_CRYPTO_MBEDTLS)
    #include <mbedtls/md5.h>
    #include <mbedtls/sha1.h>
    #include <mbedtls/sha256.h>

    typedef mbedtls_md5_context md5Context;

    static inline void md5_begin(md5Context *ctx) {
        mbedtls_md5_init(ctx);
        mbedtls_md5_starts(ctx);
    }
    static inline void md5_add(md5Context *ctx, const void *bytes, size_t length) {
        mbedtls_md5_update(ctx, (const unsigned char*)bytes, length);
    }
    static inline void md5_end(md5Context *ctx, void *outDigest) {
        mbedtls_md5_finish(ctx, (unsigned char*)outDigest);
        mbedtls_md5_free(ctx);
    }


    typedef mbedtls_sha1_context sha1Context;

    static inline void sha1_begin(sha1Context *ctx) {
        mbedtls_sha1_init(ctx);
        mbedtls_sha1_starts(ctx);
    }
    static inline void sha1_add(sha1Context *ctx, const void *bytes, size_t length) {
        mbedtls_sha1_update(ctx, (unsigned char*)bytes, length);
    }
    static inline void sha1_end(sha1Context *ctx, void *outDigest) {
        mbedtls_sha1_finish(ctx, (unsigned char *)outDigest);
        mbedtls_sha1_free(ctx);
    }


    typedef mbedtls_sha256_context sha256Context;

    static inline void sha256_begin(sha256Context *ctx) {
        mbedtls_sha256_init(ctx);
        mbedtls_sha256_starts(ctx, 0);
    }
    static inline void sha256_add(sha256Context *ctx, const void *bytes, size_t length) {
        mbedtls_sha256_update(ctx, (unsigned char *)bytes, length);
    }
    static inline void sha256_end(sha256Context *ctx, void *outDigest) {
        mbedtls_sha256_finish(ctx, (unsigned char *)outDigest);
        mbedtls_sha256_free(ctx);
    }
    
    #define SECURE_DIGEST_AVAILABLE 1


#else

    #define SECURE_DIGEST_AVAILABLE 0

#endif


#if SECURE_DIGEST_AVAILABLE
namespace litecore {

    struct SHA1 {
        char bytes[20];

        SHA1(fleece::slice s) {
            sha1Context context;
            sha1_begin(&context);
            sha1_add(&context, s.buf, s.size);
            sha1_end(&context, this);
        }
    };

}
#endif


