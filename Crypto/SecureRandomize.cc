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
#include <random>

#ifdef __APPLE__
    #include <CommonCrypto/CommonRandom.h>
#else
    #include "mbedUtils.hh"
    #include "mbedtls/ctr_drbg.h"
#endif


namespace litecore {

    std::random_device rd;
    std::default_random_engine e(rd());

    uint32_t RandomNumber() {
        return e();
    }

    uint32_t RandomNumber(uint32_t upperBound) {
        std::uniform_int_distribution<uint32_t> uniform(0, upperBound - 1);
        return uniform(e);
    }


    void GenerateUUID(fleece::mutable_slice s) {
        // https://en.wikipedia.org/wiki/Universally_unique_identifier#Version_4_.28random.29
        Assert(s.size == SizeOfUUID);
        SecureRandomize(s);
        auto bytes = (uint8_t*)s.buf;
        bytes[6] = (bytes[6] & ~0xF0) | 0x40;    // Set upper 4 bits to 0100
        bytes[8] = (bytes[8] & ~0xC0) | 0x80;    // Set upper 2 bits to 10
    }


    void SecureRandomize(fleece::mutable_slice s) {
#ifdef __APPLE__
        // iOS and Mac OS implementation based on system-level CommonCrypto library:
        CCRandomGenerateBytes(s.buf, s.size);
#else
        // Other platforms use mbedTLS crypto:
        mbedtls_ctr_drbg_random(crypto::RandomNumberContext(), (unsigned char*)s.buf, s.size);
#endif
    }

}
