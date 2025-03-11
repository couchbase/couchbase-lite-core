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
#include <cstdint>

namespace litecore {

    /** Fills memory with cryptographically-secure random bytes. */
    void SecureRandomize(fleece::mutable_slice);

    /** Returns a random integer in the range [0 .. UINT32_MAX].
        @warning  On some platforms this RNG is cryptographically secure, on others less so. */
    uint32_t RandomNumber();

    /** Returns a random integer in the range [0 .. upperBound-1].
        @warning  On some platforms this RNG is cryptographically secure, on others less so. */
    uint32_t RandomNumber(uint32_t upperBound);
}  // namespace litecore
