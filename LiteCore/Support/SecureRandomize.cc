//
// SecureRandomize.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "SecureRandomize.hh"
#include "Error.hh"
#include "PlatformCompat.hh"

#ifndef __APPLE__
#include "arc4random.h"
#endif

#if defined(_CRYPTO_CC)
    #include <CommonCrypto/CommonCryptor.h>
    #include <CommonCrypto/CommonRandom.h>
#elif defined(_CRYPTO_LIBTOMCRYPT)
    #include <tomcrypt.h>
#elif defined(_CRYPTO_OPENSSL)
    #include <openssl/rand.h>
#elif defined(_CRYPTO_MBEDTLS)
    #include <mbedtls/entropy.h>
    #include <mbedtls/ctr_drbg.h>
    #include <mutex>
    #if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        #include <Windows.h>
        #include <bcrypt.h>
    #endif
#endif


namespace litecore {

    uint32_t RandomNumber() {
        return arc4random();
    }

    uint32_t RandomNumber(uint32_t upperBound) {
        return arc4random_uniform(upperBound);
    }


    void GenerateUUID(fleece::slice s) {
        // https://en.wikipedia.org/wiki/Universally_unique_identifier#Version_4_.28random.29
        Assert(s.size == SizeOfUUID);
        SecureRandomize(s);
        auto bytes = (uint8_t*)s.buf;
        bytes[6] = (bytes[6] & ~0xF0) | 0x40;    // Set upper 4 bits to 0100
        bytes[8] = (bytes[8] & ~0xC0) | 0x80;    // Set upper 2 bits to 10
    }


#if defined(_CRYPTO_CC)

    // iOS and Mac OS implementation based on system-level CommonCrypto library:
        void SecureRandomize(fleece::slice s) {
            CCRandomGenerateBytes((void*)s.buf, s.size);
        }

//#elif defined(_CRYPTO_LIBTOMCRYPT)
//
//    // TODO: Support TomCrypt

#elif defined(_CRYPTO_OPENSSL)

        void SecureRandomize(fleece::slice s) {
            RAND_bytes((unsigned char *)s.buf, (int)s.size);
        }

#elif defined(_CRYPTO_MBEDTLS)

    void SecureRandomize(fleece::slice s) {
        static std::once_flag f;
        static mbedtls_entropy_context entropy;
        static mbedtls_ctr_drbg_context ctr_drbg;

        std::call_once(f, [] {
            mbedtls_entropy_init(&entropy);
    #if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
            auto uwp_entropy_poll = [](void *data, unsigned char *output, size_t len,
                                       size_t *olen) -> int
            {
                NTSTATUS status = BCryptGenRandom(NULL, output, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
                if (status < 0) {
                    return(MBEDTLS_ERR_ENTROPY_SOURCE_FAILED);
                }

                *olen = len;
                return 0;
            };
            mbedtls_entropy_add_source(&entropy, uwp_entropy_poll, NULL, 32,
                                       MBEDTLS_ENTROPY_SOURCE_STRONG);
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

#else

    #warn SecureRandomize is not available

    void SecureRandomize(fleece::slice s) {
        error::_throw(error::Unimplemented);
    }

#endif

}
