//
//  Base.h
//  CBNano
//
//  Created by Jens Alfke on 7/21/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef Base_h
#define Base_h

#include "slice.hh"


namespace cbforest {
    typedef fleece::slice slice;
    typedef fleece::alloc_slice alloc_slice;

    typedef uint64_t sequence;
}


#endif /* Base_h */
