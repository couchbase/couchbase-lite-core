//
//  SecureDigest.hh
//  CBForest
//
//  Created by Jens Alfke on 12/29/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef SecureDigest_hh
#define SecureDigest_hh


#if defined(_CRYPTO_CC)

    // iOS and Mac OS implementation based on system-level CommonCrypto library:
    #include <CommonCrypto/CommonDigest.h>
    #include <assert.h>

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

    #define SECURE_DIGEST_AVAILABLE 1

#elif defined(_CRYPTO_LIBTOMCRYPT)

    #include <tomcrypt.h>
    // TODO
    #define SECURE_DIGEST_AVAILABLE 0

#elif defined(_CRYPTO_OPENSSL)

    #include <openssl/aes.h>
    // TODO
    #define SECURE_DIGEST_AVAILABLE 0

#else

    #define SECURE_DIGEST_AVAILABLE 0

#endif


#endif /* SecureDigest_hh */
