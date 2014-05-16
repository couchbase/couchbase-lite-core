//
//  VersionedDocument_Tests.mm
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "VersionedDocument.h"
#import "testutil.h"

using namespace forestdb;

@interface VersionedDocument_Tests : XCTestCase
@end

@implementation VersionedDocument_Tests

- (void)setUp
{
    [super setUp];
    // Put setup code here. This method is called before the invocation of each test method in the class.
}

- (void)tearDown
{
    // Put teardown code here. This method is called after the invocation of each test method in the class.
    [super tearDown];
}

- (void)testExample
{
    RevTree tree;
    const RevNode* rev;
    forestdb::slice rev1ID("1-aaaa");
    forestdb::slice rev1Data("body of revision");
    rev = tree.insert(rev1ID, rev1Data, false,
                      forestdb::slice::null, false);
    Assert(rev);
    Assert(rev->revID.equal(rev1ID));
    Assert(rev->data.equal(rev1Data));
    AssertEq(rev->parentIndex, RevNode::kNoParent);
    AssertEq(rev->flags, 0);
}

@end
