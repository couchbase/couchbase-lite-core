//
// SecureRandomize.hh
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include <climits>
#include <cstdint>
#include <memory>

namespace litecore {

    /** Fills memory with cryptographically-secure random bytes. */
    void SecureRandomize(fleece::mutable_slice);

    /** Returns a random integer in the range [0 .. UINT32_MAX].
        @warning  On some platforms this RNG is cryptographically secure, on others less so. */
    uint32_t RandomNumber();

    /** Returns a random integer in the range [0 .. upperBound-1].
        @warning  On some platforms this RNG is cryptographically secure, on others less so. */
    uint32_t RandomNumber(uint32_t upperBound);

    /** Random number generator class that can be used to abstract the source/type of randomness.
        Default implementation uses `RandomNumber()`. Can be subclassed to use other generators. */
    class RandomNumberGenerator {
      public:
        // This class conforms to the concept `std::uniform_random_bit_generator`.
        using result_type = uint32_t;

        static constexpr result_type min() { return 0; }

        static constexpr result_type max() { return UINT_MAX; }

        virtual result_type operator()() { return RandomNumber(); };

        virtual ~RandomNumberGenerator() = default;

        /// The default instance, which uses `std::random_device`.
        /// @warning  On some platforms this RNG is cryptographically secure, on others less so.
        static RandomNumberGenerator& defaultInstance();

        /// Creates a RandomNumberGenerator that generates pseudo-random numbers based on a seed.
        /// The same seed always creates the same sequence of numbers.
        static std::unique_ptr<RandomNumberGenerator> newRepeatable(uint32_t seed);

        /// Returns a random integer in the range [0 .. UINT32_MAX].
        uint32_t randomNumber() { return (*this)(); }

        /// Returns a random integer in the range [0 .. upperBound-1].
        uint32_t randomNumber(uint32_t upperBound);

        /// Returns a random real number in the range [lowerBound .. upperBound).
        double randomDouble(double lowerBound, double upperBound);

        /// Returns a random real number with a normal/Gaussian distribution.
        /// <https://en.wikipedia.org/wiki/Normal_distribution>
        /// About 68% of results will be within mean ± stdDev.
        /// About 95% will be within ± 2 times stdDev; 99.9% within ± 3 times stdDev.
        /// @param mean  The mean (average) value to return.
        /// @param stdDev  The standard deviation.
        double randomNormalDouble(double mean, double stdDev);
    };

}  // namespace litecore
