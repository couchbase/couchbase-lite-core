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

#pragma mark - RANDOM NUMBER GENERATOR:

    RandomNumberGenerator& RandomNumberGenerator::defaultInstance() {
        static RandomNumberGenerator* sRNG = new RandomNumberGenerator;
        return *sRNG;
    }

    uint32_t RandomNumberGenerator::randomNumber(uint32_t upperBound) {
        std::uniform_int_distribution<uint32_t> uniform(0, upperBound - 1);
        return uniform(*this);
    }

    double RandomNumberGenerator::randomDouble(double lowerBound, double upperBound) {
        std::uniform_real_distribution<double> uniform(lowerBound, upperBound);
        return uniform(*this);
    }

    double RandomNumberGenerator::randomNormalDouble(double mean, double stdDev) {
        std::normal_distribution norm(mean, stdDev);
        return norm(*this);
    }

    class RepeatableRandomNumberGenerator : public RandomNumberGenerator {
      public:
        explicit RepeatableRandomNumberGenerator(uint32_t seed) : _rng(seed) {}

        result_type operator()() override { return _rng(); };

      private:
        std::mt19937 _rng;
    };

    std::unique_ptr<RandomNumberGenerator> RandomNumberGenerator::newRepeatable(uint32_t seed) {
        return std::make_unique<RepeatableRandomNumberGenerator>(seed);
    }

#pragma mark - SECURE RANDOMIZE:

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
