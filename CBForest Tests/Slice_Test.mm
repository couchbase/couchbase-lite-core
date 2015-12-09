//
//  Slice_Test.mm
//  CBForest
//
//  Created by Pasin Suriyentrakorn on 8/14/15.
//  Copyright (c) 2015 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "testutil.h"
#import "slice.hh"

using namespace cbforest;

@interface Slice_Test : XCTestCase
@end

@implementation Slice_Test

- (void) testHasPrefix {
    slice s = slice();
    Assert(!s.hasPrefix(NULL));
    Assert(!s.hasPrefix(slice()));
    Assert(!s.hasPrefix(slice("")));
    Assert(!s.hasPrefix(slice("abc")));
    
    s = slice("");
    Assert(!s.hasPrefix(NULL));
    Assert(!s.hasPrefix(slice()));
    Assert(!s.hasPrefix(slice("")));
    Assert(!s.hasPrefix(slice("abc")));
    
    s = slice("abc");
    Assert(s.hasPrefix(slice("ab")));
    Assert(s.hasPrefix(slice("abc")));
    Assert(!s.hasPrefix(NULL));
    Assert(!s.hasPrefix(slice()));
    Assert(!s.hasPrefix(slice("")));
    Assert(!s.hasPrefix(slice("ac")));
    Assert(!s.hasPrefix(slice("abcd")));
}

- (void) testAllocSliceToNSData {
    const void *blockStart;
    @autoreleasepool {
        NSData *adopter;
        {
            alloc_slice s(1024);
            Assert(s.buf != NULL);
            AssertEq(s.size, 1024);
            memset((void*)s.buf, '!', 1024);
            blockStart = s.buf;
            adopter = s.convertToNSData();
        }

        AssertEq(adopter.bytes, blockStart);
        AssertEq(adopter.length, 1024);
        for (int i=0; i<1024; i++)
            AssertEq(((char*)blockStart)[i], '!');
    }
    // By here the block should have been freed (but there's not a good way to test for that!)
}

@end
