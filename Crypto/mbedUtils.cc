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
#include "Logging.hh"
#include "StringUtil.hh"
#include "entropy.h"
#include "pem.h"
#include <mutex>

namespace litecore { namespace crypto {
    using namespace std;
    using namespace fleece;

    [[noreturn]] void throwMbedTLSError(int err) {
        char description[100];
        mbedtls_strerror(err, description, sizeof(description));
        WarnError("mbedTLS error %s0x%x: %s", (err < 0 ? "-" : ""), abs(err), description);
        error::_throw(error::MbedTLS, err);
    }


    string getX509Name(mbedtls_x509_name *xname) {
        char nameBuf[256];
        TRY( mbedtls_x509_dn_gets(nameBuf, sizeof(nameBuf), xname) );
        return string(nameBuf);
    }


    mbedtls_ctr_drbg_context* RandomNumberContext() {
        static mbedtls_entropy_context sEntropyContext;
        static mbedtls_ctr_drbg_context sRandomNumberContext;
        static const char *kPersonalization = "LiteCore";

        static once_flag f;
        call_once(f, []() {
            // One-time initializations:
            Log( "Seeding the mbedTLS random number generator..." );
            mbedtls_ctr_drbg_init( &sRandomNumberContext );
            mbedtls_entropy_init( &sEntropyContext );
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


    alloc_slice convertToPEM(const alloc_slice &data, const char *name NONNULL)
    {
        if (data.hasPrefix("-----"_sl)) {
            // It's already PEM...
            return data;
        }

        return allocString(10000, [&](char *buf, size_t size) {
            size_t olen = 0;
            int err = mbedtls_pem_write_buffer(format("-----BEGIN %s-----\n", name).c_str(),
                                               format("-----END %s-----\n", name).c_str(),
                                               (const uint8_t*)data.buf, data.size,
                                               (uint8_t*)buf, size, &olen);
            return err ? err : (int)olen;
        });
    }

} }
