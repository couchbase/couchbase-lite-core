//
//  slice.mm
//  CBForest
//
//  Created by Jens Alfke on 5/14/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "slice.hh"
#import <Foundation/Foundation.h>

namespace forestdb {

    NSData* slice::copiedNSData() const {
        return buf ? [NSData dataWithBytes: buf length: size] : nil;
    }

    NSData* slice::uncopiedNSData() const {
        if (!buf)
            return nil;
        return [[NSData alloc] initWithBytesNoCopy: (void*)buf length: size freeWhenDone: NO];
    }


    slice::operator NSString*() const {
        if (!buf)
            return nil;
        return [[NSString alloc] initWithBytes: buf length: size encoding: NSUTF8StringEncoding];
    }


    nsstring_slice::nsstring_slice(__unsafe_unretained NSString* str)
    :_needsFree(false)
    {
        NSUInteger byteCount;
        if (str.length <= sizeof(_local)) {
            if (!str)
                return;
            // First try to copy the UTF-8 into a smallish stack-based buffer:
            NSRange remaining;
            BOOL ok = [str getBytes: _local maxLength: sizeof(_local) usedLength: &byteCount
                           encoding: NSUTF8StringEncoding options: 0
                              range: NSMakeRange(0, str.length) remainingRange: &remaining];
            if (ok && remaining.length == 0) {
                buf = &_local;
                size = byteCount;
                return;
            }
        }

        // Otherwise malloc a buffer to copy the UTF-8 into:
        NSUInteger maxByteCount = [str maximumLengthOfBytesUsingEncoding: NSUTF8StringEncoding];
        buf = ::malloc(maxByteCount);
        if (!buf)
            throw std::bad_alloc();
        _needsFree = true;
        BOOL ok = [str getBytes: (void*)buf maxLength: maxByteCount usedLength: &byteCount
                       encoding: NSUTF8StringEncoding options: 0
                          range: NSMakeRange(0, str.length) remainingRange: NULL];
        assert(ok);
        size = byteCount;
    }

    nsstring_slice::~nsstring_slice() {
        if (_needsFree)
            ::free((void*)buf);
    }


}