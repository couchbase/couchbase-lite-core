//
// Base.hh
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

#pragma once

#include "fleece/slice.hh"
#include "PlatformCompat.hh"
#include "function_ref.hh"
#include "RefCounted.hh"
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

    // Database sequence number
    typedef uint64_t sequence_t;

    enum EncryptionAlgorithm : uint8_t {
        kNoEncryption = 0,      /**< No encryption (default) */
        kAES256,                /**< AES with 256-bit key */
        kAES128,                /**< AES with 128-bit key */

        kNumEncryptionAlgorithms
    };

    constexpr size_t kEncryptionKeySize[kNumEncryptionAlgorithms] = {0, 32, 16};

}

