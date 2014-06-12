//
//  VersionedDocument_Tests.mm
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "VersionedDocument.hh"
#import "testutil.h"

using namespace forestdb;

@interface VersionedDocument_Tests : XCTestCase
@end

@implementation VersionedDocument_Tests
{
    Database* db;
}

#define kDBPath "/tmp/forest.db"


static revidBuffer stringToRev(NSString* str) {
    revidBuffer buf(str);
    return buf;
}

- (void)setUp
{
    ::unlink(kDBPath);
    [super setUp];
    db = new Database(kDBPath, FDB_OPEN_FLAG_CREATE, Database::defaultConfig());
}

- (void)tearDown
{
    delete db;
    [super tearDown];
}


- (void) test01_Empty {
    VersionedDocument v(db, @"foo");
    AssertEqual((NSString*)v.docID(), @"foo");
    Assert(v.revID() == NULL);
    AssertEq(v.flags(), 0);
    XCTAssert(v.get(stringToRev(@"1-aaaa")) == NULL);
}


- (void) test02_RevTreeInsert {
    RevTree tree;
    const RevNode* rev;
    revidBuffer rev1ID(forestdb::slice("1-aaaa"));
    forestdb::slice rev1Data("body of revision");
    int httpStatus;
    rev = tree.insert(rev1ID, rev1Data, false,
                      revid(), false, httpStatus);
    Assert(rev);
    AssertEq(httpStatus, 201);
    Assert(rev->revID.equal(rev1ID));
    Assert(rev->body.equal(rev1Data));
    AssertEq(rev->parentIndex, RevNode::kNoParent);
    Assert(!rev->isDeleted());

    revidBuffer rev2ID(forestdb::slice("2-bbbb"));
    forestdb::slice rev2Data("second revision");
    auto rev2 = tree.insert(rev2ID, rev2Data, false, rev1ID, false, httpStatus);
    Assert(rev2);
    AssertEq(httpStatus, 201);
    Assert(rev2->revID.equal(rev2ID));
    Assert(rev2->body.equal(rev2Data));
    Assert(!rev->isDeleted());

    tree.sort();
    rev = tree.get(rev1ID);
    rev2 = tree.get(rev2ID);
    Assert(rev);
    Assert(rev2);
    AssertEq(rev2->parent(), rev);
    Assert(rev->parent() == NULL);

    AssertEq(tree.currentNode(), rev2);
    Assert(!tree.hasConflict());

    tree.sort();
    AssertEq(tree[0], rev2);
    AssertEq(tree[1], rev);
    AssertEq(rev->index(), 1);
    AssertEq(rev2->index(), 0);

    alloc_slice ext = tree.encode();

    RevTree tree2 = RevTree(ext, 12, 1234);
}

- (void) test03_AddRevision {
    NSString *revID = @"1-fadebead", *body = @"{\"hello\":true}";
    revidBuffer revIDBuf(revID);
    VersionedDocument v(db, @"foo");
    int httpStatus;
    v.insert(revIDBuf, body, false, NULL, false, httpStatus);
    AssertEq(httpStatus, 201);

    const RevNode* node = v.get(stringToRev(revID));
    Assert(node);
    Assert(!node->isDeleted());
    Assert(node->isLeaf());
    Assert(node->isActive());
    AssertEq(v.size(), 1);
    AssertEq(v.currentNodes().size(), 1);
    AssertEq(v.currentNodes()[0], v.currentNode());
}

@end
