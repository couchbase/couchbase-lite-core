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

@end
