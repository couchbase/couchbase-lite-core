//
//  SecureRandomize.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 12/28/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include "Error.hh"
#include "PlatformCompat.hh"

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
        RAND_bytes((unsigned char *)s.buf, (int)s.size);
    }

    #define SECURE_RANDOMIZE_AVAILABLE 1
    
#elif defined(_CRYPTO_MBEDTLS)

    #include <mbedtls/entropy.h>
    #include <mbedtls/ctr_drbg.h>
    #include <mutex>

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP)
    #include <Windows.h>
    #include <bcrypt.h>

    static int uwp_entropy_poll(void *data, unsigned char *output, size_t len,
        size_t *olen)
    {
        NTSTATUS status = BCryptGenRandom(NULL, output, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            return(MBEDTLS_ERR_ENTROPY_SOURCE_FAILED);
        }

        return 0;
    }
#endif

    static int initialized = 0;
    static mbedtls_entropy_context entropy;
    static mbedtls_ctr_drbg_context ctr_drbg;
    static inline void SecureRandomize(fleece::slice s) {
        once_flag f;
        call_once(f, [=] {
            mbedtls_entropy_init(&entropy);
#ifdef WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP)
            mbedtls_entropy_add_source(&entropy, uwp_entropy_poll, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
#endif
            mbedtls_ctr_drbg_init(&ctr_drbg);
            const unsigned char* val = (const unsigned char *)"Salty McNeal";
            int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, val, strlen((const char*)val));
            if (ret != 0) {
                litecore::error::_throw(litecore::error::CryptoError);
            }
        });

        mbedtls_ctr_drbg_random(&ctr_drbg, (unsigned char*)s.buf, s.size);
    }
    
    #define SECURE_RANDOMIZE_AVAILABLE 1


#else

    static inline void SecureRandomize(fleece::slice s) {
        error::_throw(error::Unimplemented);
    }

    #define SECURE_RANDOMIZE_AVAILABLE 0

#endif


namespace litecore {

    static const size_t SizeOfUUID = 32;
    void GenerateUUID(fleece::slice);

}
