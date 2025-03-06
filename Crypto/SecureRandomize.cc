//
// SecureRandomize.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SecureRandomize.hh"
#include "Error.hh"
#include "SecureDigest.hh"
#include <random>

#ifdef __APPLE__
#    include <CommonCrypto/CommonRandom.h>
#else
#    include "mbedUtils.hh"
#    include "mbedtls/ctr_drbg.h"
#endif


namespace litecore {

    static std::random_device         rd;
    static std::default_random_engine e(rd());

    uint32_t RandomNumber() { return e(); }

    uint32_t RandomNumber(uint32_t upperBound) {
        std::uniform_int_distribution<uint32_t> uniform(0, upperBound - 1);
        return uniform(e);
    }

    static void stampUUID(void* out, uint8_t version) {
        auto bytes = (uint8_t*)out;
        bytes[6]   = (bytes[6] & ~0xF0) | uint8_t(version << 4);
        bytes[8]   = (bytes[8] & ~0xC0) | 0x80;
    }

    void GenerateUUID(fleece::mutable_slice out) {
        // https://en.wikipedia.org/wiki/Universally_unique_identifier#Version_4_.28random.29
        Assert(out.size == SizeOfUUID);
        SecureRandomize(out);
        stampUUID(out.buf, 4);
    }

    void GenerateNamespacedUUID(fleece::mutable_slice out, fleece::slice namespaceUUID, fleece::slice name) {
        // https://datatracker.ietf.org/doc/html/rfc9562#name-uuid-version-5
        Assert(out.size == SizeOfUUID);
        Assert(namespaceUUID.size == SizeOfUUID);
        SHA1 digest = (SHA1Builder{} << namespaceUUID << name).finish();
        memcpy(out.buf, &digest, SizeOfUUID);  // copy first 128 bits of SHA-1
        stampUUID(out.buf, 5);
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

}  // namespace litecore
