//
// mbedUtils.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "mbedUtils.hh"
#include "Error.hh"
#include "Logging.hh"
#include "StringUtil.hh"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation-deprecated-sync"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pem.h"
#include "mbedtls/x509_crt.h"
#pragma clang diagnostic pop

#include <mutex>

#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    #include <Windows.h>
    #include <bcrypt.h>
#endif

namespace litecore { namespace crypto {
    using namespace std;
    using namespace fleece;

    [[noreturn]] void throwMbedTLSError(int err) {
        char description[100];
        mbedtls_strerror(err, description, sizeof(description));
        WarnError("mbedTLS error %s0x%x: %s", (err < 0 ? "-" : ""), abs(err), description);
        error::_throw(error::MbedTLS, err);
    }


    alloc_slice getX509Name(mbedtls_x509_name *xname) {
        char nameBuf[256];
        TRY( mbedtls_x509_dn_gets(nameBuf, sizeof(nameBuf), xname) );
        return alloc_slice(nameBuf);
    }


    mbedtls_ctr_drbg_context* RandomNumberContext() {
        static mbedtls_entropy_context sEntropyContext;
        static mbedtls_ctr_drbg_context sRandomNumberContext;
        static const char *kPersonalization = "LiteCore";

        static once_flag f;
        call_once(f, []() {
            // One-time initializations:
            Log( "Seeding the mbedTLS random number generator..." );
            mbedtls_entropy_init( &sEntropyContext );

#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
            auto uwp_entropy_poll = [](void *data, unsigned char *output, size_t len,
                                       size_t *olen) -> int
            {
                NTSTATUS status = BCryptGenRandom(NULL, output, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
                if (status < 0) {
                    return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
                }

                *olen = len;
                return 0;
            };
            mbedtls_entropy_add_source(&sEntropyContext, uwp_entropy_poll, NULL, 32,
                                       MBEDTLS_ENTROPY_SOURCE_STRONG);
#endif

            mbedtls_ctr_drbg_init( &sRandomNumberContext );
            TRY( mbedtls_ctr_drbg_seed(&sRandomNumberContext,
                                       mbedtls_entropy_func, &sEntropyContext,
                                       (const unsigned char *) kPersonalization,
                                       strlen(kPersonalization)) );
        });
        return &sRandomNumberContext;
    }


    alloc_slice allocString(size_t maxSize, function_ref<int(char*,size_t)> writer) {
        alloc_slice data(maxSize);
        int len = TRY( writer((char*)data.buf, data.size) );
        Assert(len <= maxSize);
        data.resize(len);
        return data;
    }


    alloc_slice allocDER(size_t maxSize, function_ref<int(uint8_t*,size_t)> writer) {
        alloc_slice data(maxSize);
        int len = TRY( writer((uint8_t*)data.buf, data.size) );
        Assert(len <= maxSize);
        memmove((char*)&data[0], &data[data.size - len], len);
        data.resize(len);
        return data;
    }


    void parsePEMorDER(slice data, const char *what, ParseCallback fn) {
        int err;
        if (data.containsBytes("-----BEGIN "_sl) && !data.hasSuffix('\0')) {
            // mbedTLS requires a null byte at the end of PEM data:
            alloc_slice adjustedData(data);
            adjustedData.resize(data.size + 1);
            *((char*)adjustedData.end() - 1) = '\0';
            
            err = fn((const uint8_t*)adjustedData.buf, adjustedData.size);
        } else {
            err = fn((const uint8_t*)data.buf, data.size);
        }
        
        if (err != 0) {
            char buf[100];
            mbedtls_strerror(err, buf, sizeof(buf));
            error::_throw(error::CryptoError, "Can't parse %s data (%s)", what, buf);
        }
    }


    alloc_slice convertToPEM(const slice &data, const char *name NONNULL) {
        return allocString(10000, [&](char *buf, size_t size) {
            size_t olen = 0;
            int err = mbedtls_pem_write_buffer(format("-----BEGIN %s-----\n", name).c_str(),
                                               format("-----END %s-----\n", name).c_str(),
                                               (const uint8_t*)data.buf, data.size,
                                               (uint8_t*)buf, size, &olen);
            if (err != 0)
                return err;
            if (olen > 0 && buf[olen-1] == '\0')
                --olen;
            return (int)olen;
        });
    }

} }
