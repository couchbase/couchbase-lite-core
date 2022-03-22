//
// Base.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "fleece/slice.hh"
#include "fleece/PlatformCompat.hh"
#include "fleece/function_ref.hh"
#include "fleece/RefCounted.hh"
#include "c4Base.h"
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <utility>

namespace fleece {
    class slice_ostream;
}

namespace litecore {
    // For types #included above, add them to the `litecore` namespace so headers don't have to
    // use their full names. (This also keeps usages in those headers from being flagged by the
    // `missing_includes` script, which only notices types prefixed with `std::`.)
    using std::move;
    using std::pair;
    using std::string;
    using std::shared_ptr;
    using std::unique_ptr;

    using fleece::slice;
    using fleece::slice_ostream;
    using fleece::alloc_slice;
    using fleece::nullslice;
    using fleece::function_ref;
    using fleece::RefCounted;
    using fleece::Retained;

    using sequence_t = C4SequenceNumber;

    enum EncryptionAlgorithm : uint8_t {
        kNoEncryption = 0,      /**< No encryption (default) */
        kAES256,                /**< AES with 256-bit key */
        kAES128,                /**< AES with 128-bit key */

        kNumEncryptionAlgorithms
    };

    constexpr size_t kEncryptionKeySize[kNumEncryptionAlgorithms] = {0, 32, 16};

}

