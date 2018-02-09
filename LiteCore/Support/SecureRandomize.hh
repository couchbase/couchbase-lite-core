//
// SecureRandomize.hh
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

#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    #include <Windows.h>
    #include <bcrypt.h>

    static int uwp_entropy_poll(void *data, unsigned char *output, size_t len,
        size_t *olen)
    {
        NTSTATUS status = BCryptGenRandom(NULL, output, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            return(MBEDTLS_ERR_ENTROPY_SOURCE_FAILED);
        }

        *olen = len;
        return 0;
    }
#endif

    inline void SecureRandomize(fleece::slice s) {
        static std::once_flag f;
        static mbedtls_entropy_context entropy;
        static mbedtls_ctr_drbg_context ctr_drbg;

        std::call_once(f, [] {
            mbedtls_entropy_init(&entropy);
#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
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

    static const size_t SizeOfUUID = 16;
    void GenerateUUID(fleece::slice);

}
