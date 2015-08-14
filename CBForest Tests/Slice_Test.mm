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

using namespace forestdb;

@interface Slice_Test : XCTestCase
@end

@implementation Slice_Test

- (void) testHasPrefix {
    slice s = nsstring_slice(NULL);
    Assert(!s.hasPrefix(NULL));
    Assert(!s.hasPrefix(""));
    Assert(!s.hasPrefix("abc"));
    
    s = nsstring_slice(@"");
    Assert(!s.hasPrefix(NULL));
    Assert(!s.hasPrefix(""));
    Assert(!s.hasPrefix("abc"));
    
    s = nsstring_slice(@"abc");
    Assert(s.hasPrefix("ab"));
    Assert(s.hasPrefix("abc"));
    Assert(!s.hasPrefix(NULL));
    Assert(!s.hasPrefix(""));
    Assert(!s.hasPrefix("ac"));
    Assert(!s.hasPrefix("abcd"));
}

@end
