//
//  slice.h
//  CBForest
//
//  Created by Jens Alfke on 4/20/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef CBForest_slice_h
#define CBForest_slice_h

#include <stddef.h>


/** A bounded region of memory. */
typedef struct {
    const void* buf;
    size_t      size;
} slice;


/** Copies the slice into a newly malloced buffer, and returns a new slice pointing to it. */
slice slicecopy(slice buf);

/** Basic binary comparison of two slices, returning -1, 0 or 1. */
int slicecmp(slice a, slice b);


#endif
