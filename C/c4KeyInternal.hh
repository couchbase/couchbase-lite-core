//
//  c4KeyInternal.hh
//  CBForest
//
//  Created by Jens Alfke on 8/12/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef c4KeyInternal_h
#define c4KeyInternal_h

#include "c4Internal.hh"
#include "c4Key.h"
#include "Collatable.hh"


// Structs below are in the global namespace because they are forward-declared in the C API.

struct c4Key : public CollatableBuilder, c4Internal::InstanceCounted {
    c4Key()                 :CollatableBuilder() { }
    c4Key(C4Slice bytes)    :CollatableBuilder(bytes, true) { }
};


struct c4KeyValueList {
    std::vector<Collatable> keys;
    std::vector<alloc_slice> values;
};


#endif /* c4KeyInternal_h */
