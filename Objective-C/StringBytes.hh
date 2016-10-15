//
//  StringBytes.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#import <Foundation/Foundation.h>


/** A slice holding the data of an NSString. It might point directly into the NSString, so
 don't modify or release the NSString while this is in scope. Or instead it might copy
 the data into a small internal buffer, or allocate it on the heap. */
struct stringBytes  {
    stringBytes(NSString*);
    ~stringBytes();

    const char *buf;
    size_t size;

private:
    char _local[127];
    bool _needsFree;
};
