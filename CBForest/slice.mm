//
//  slice.mm
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "slice.h"
#import <Foundation/Foundation.h>

namespace forestdb {

    slice::slice(NSString* str) {
        NSData* data = [str dataUsingEncoding: NSUTF8StringEncoding];
        buf = data.bytes;
        size = data.length;
    }

    slice::operator NSData*() const {
        return buf ? [NSData dataWithBytes: buf length: size] : nil;
    }

    slice::operator NSString*() const {
        if (!buf)
            return nil;
        return [[NSString alloc] initWithBytes: buf length: size encoding: NSUTF8StringEncoding];
    }

}