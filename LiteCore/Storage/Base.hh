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


namespace litecore {
    using fleece::slice;
    using fleece::alloc_slice;
    using fleece::nullslice;

    typedef uint64_t sequence;

    typedef sequence sequence_t;    // Sometimes used for disambiguation with a sequence() method

    enum EncryptionAlgorithm : uint8_t {
        kNoEncryption = 0,      /**< No encryption (default) */
        kAES256                 /**< AES with 256-bit key */
    };

}

