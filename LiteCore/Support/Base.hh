//
//  Base.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/21/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "slice.hh"
#include "PlatformCompat.hh"
#include "make_unique.h"


namespace litecore {
    using fleece::slice;
    using fleece::alloc_slice;
    using fleece::nullslice;

    // Database sequence number
    typedef uint64_t sequence_t;

    enum EncryptionAlgorithm : uint8_t {
        kNoEncryption = 0,      /**< No encryption (default) */
        kAES256                 /**< AES with 256-bit key */
    };

}

