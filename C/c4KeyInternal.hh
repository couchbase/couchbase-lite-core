//
//  c4KeyInternal.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "c4Internal.hh"
#include "c4Key.h"
#include "Collatable.hh"


// Structs below are in the global namespace because they are forward-declared in the C API.

struct c4Key : public CollatableBuilder, InstanceCounted {
    c4Key()                 :CollatableBuilder() { }
    c4Key(C4Slice bytes)    :CollatableBuilder(bytes, true) { }
};


struct c4KeyValueList {
    vector<Collatable> keys;
    vector<alloc_slice> values;
};

