//
//  Base.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/21/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "slice.hh"


namespace CBL_Core {
    using fleece::slice;
    using fleece::alloc_slice;

    typedef uint64_t sequence;

    typedef sequence sequence_t;    // Sometimes used for disambiguation with a sequence() method
}

