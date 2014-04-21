//
//  sized_buf.h
//  CBForest
//
//  Created by Jens Alfke on 4/20/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef CBForest_sized_buf_h
#define CBForest_sized_buf_h

#include <stddef.h>


typedef struct {
    void* buf;
    size_t size;
} sized_buf;


#endif
