//
//  SecureRandomize.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 12/28/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"


#if defined(_CRYPTO_CC)

    // iOS and Mac OS implementation based on system-level CommonCrypto library:
    #include <CommonCrypto/CommonCryptor.h>
    #include <CommonCrypto/CommonRandom.h>

    static inline void SecureRandomize(fleece::slice s) {
        CCRandomGenerateBytes((void*)s.buf, s.size);
    }

    #define SECURE_RANDOMIZE_AVAILABLE 1

#elif defined(_CRYPTO_LIBTOMCRYPT)

    #include <tomcrypt.h>
    // TODO
    #define SECURE_RANDOMIZE_AVAILABLE 0

#elif defined(_CRYPTO_OPENSSL)

    #include <openssl/rand.h>

    static inline void SecureRandomize(fleece::slice s) {
        RAND_bytes((unsigned char *)s.buf, s.size);
    }

    #define SECURE_RANDOMIZE_AVAILABLE 1

#else

    #define SECURE_RANDOMIZE_AVAILABLE 0

#endif

