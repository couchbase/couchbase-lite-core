//
//  SecureDigest.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 12/29/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
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

#else

    #define SECURE_DIGEST_AVAILABLE 0

#endif


#if SECURE_DIGEST_AVAILABLE
namespace litecore {

    struct SHA1 {
        char bytes[20];

        SHA1(slice s) {
            sha1Context context;
            sha1_begin(&context);
            sha1_add(&context, s.buf, s.size);
            sha1_end(&context, this);
        }
    };

}
#endif


