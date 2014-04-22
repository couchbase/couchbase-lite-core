//
//  slice.c
//  CBForest
//
//  Created by Jens Alfke on 4/21/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "slice.h"
#include <stdlib.h>
#include <string.h>


slice slicecopy(slice buf) {
    if (buf.size > 0) {
        void* newBuf = malloc(buf.size);
        memcpy(newBuf, buf.buf, buf.size);
        buf.buf = newBuf;
    }
    return buf;
}


int slicecmp(slice a, slice b) {
    size_t minSize = a.size < b.size ? a.size : b.size;
    int result = memcmp(a.buf, b.buf, minSize);
    if (result == 0) {
        if (a.size < b.size)
            result = -1;
        else if (a.size > b.size)
            result = 1;
    }
    return result;
}
