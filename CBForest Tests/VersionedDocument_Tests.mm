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

- (void)testRevTreeInsert
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
    Assert(!rev->isDeleted());

    forestdb::slice rev2ID("2-bbbb");
    forestdb::slice rev2Data("second revision");
    auto rev2 = tree.insert(rev2ID, rev2Data, false, rev1ID, false);
    Assert(rev2);
    Assert(rev2->revID.equal(rev2ID));
    Assert(rev2->data.equal(rev2Data));
    Assert(!rev->isDeleted());

    tree.sort();
    rev = tree.get(rev1ID);
    rev2 = tree.get(rev2ID);
    Assert(rev);
    Assert(rev2);
    AssertEq(tree.parentNode(rev2), rev);
    Assert(tree.parentNode(rev) == NULL);

    AssertEq(tree.currentNode(), rev2);
    Assert(!tree.hasConflict());

    tree.sort();
    AssertEq(tree[0], rev2);
    AssertEq(tree[1], rev);
    AssertEq(tree.indexOf(rev), 1);
    AssertEq(tree.indexOf(rev2), 0);

    alloc_slice ext = tree.encode();

    RevTree tree2 = RevTree(ext, 12, 1234);
}

@end
