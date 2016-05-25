//
//  SecureRandomize.hh
//  CBForest
//
//  Created by Jens Alfke on 12/28/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef SecureRandomize_h
#define SecureRandomize_h


#if defined(_CRYPTO_CC)

    // iOS and Mac OS implementation based on system-level CommonCrypto library:
    #include <CommonCrypto/CommonCryptor.h>
    #include <CommonCrypto/CommonRandom.h>

    static inline void SecureRandomize(slice s) {
        CCRandomGenerateBytes((void*)s.buf, s.size);
    }

    #define SECURE_RANDOMIZE_AVAILABLE 1

#elif defined(_CRYPTO_LIBTOMCRYPT)

    #include <tomcrypt.h>
    // TODO
    #define SECURE_RANDOMIZE_AVAILABLE 0

#elif defined(_CRYPTO_OPENSSL)

    #include <openssl/rand.h>

    static inline void SecureRandomize(slice s) {
        RAND_bytes((unsigned char *)s.buf, s.size);
    }

    #define SECURE_RANDOMIZE_AVAILABLE 1

#else

    #define SECURE_RANDOMIZE_AVAILABLE 0

#endif


#endif /* SecureRandomize_h */
