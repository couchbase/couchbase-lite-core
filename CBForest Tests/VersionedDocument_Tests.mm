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

using namespace cbforest;

@interface VersionedDocument_Tests : XCTestCase
@end

@implementation VersionedDocument_Tests
{
    std::string dbPath;
    Database* db;
}


static revidBuffer stringToRev(NSString* str) {
    revidBuffer buf(str);
    return buf;
}

+ (void) initialize {
    if (self == [VersionedDocument_Tests class]) {
        LogLevel = kWarning;
    }
}

- (void)setUp
{
    [super setUp];
    CreateTestDir();
    dbPath = PathForDatabaseNamed(@"forest_temp.fdb");
    db = new Database(dbPath, TestDBConfig());
}

- (void)tearDown
{
    delete db;
    [super tearDown];
}


- (void) test00_RevIDs {
    revidBuffer rev(@"1-f0f0");
    AssertEqual((NSString*)rev, @"1-f0f0");
    static const uint8_t expectedBytes[] = {0x01, 0xf0, 0xf0};
    Assert(rev == slice(expectedBytes, sizeof(expectedBytes)));

    rev = stringToRev(@"1234-1234567890abcdef1234567890abcdef");
    AssertEqual((NSString*)rev, @"1234-1234567890abcdef1234567890abcdef");
    static const uint8_t expectedBytes2[18] = {0xd2, 0x09, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD,
        0xEF, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF};
    Assert(rev == slice(expectedBytes2, sizeof(expectedBytes2)));

    // New-style ('clock') revID:
    rev.parseNew(slice("17@snej"));
    Assert(rev.isClock());
    AssertEq(rev.generation(), 17);
    AssertEq(rev.digest(), slice("snej"));
    static const uint8_t expectedBytes3[] = {0x00, 0x11, 's', 'n', 'e', 'j'};
    Assert(rev == slice(expectedBytes3, sizeof(expectedBytes3)));
}

- (void) test00_BadRevIDs {
    // Check a bunch of invalid revIDs to make sure they all correctly fail to parse:
    static const char* kBadStrings[] = {
        "",
        "@",
        "1@",
        "@snej",
        "snej@x",
        "0@snej"
        "1234567890@snej",
        "1234@abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
        "-",
        "1-",
        "-ff",
        "1-snej",
        "1-abc",
        "0-cafe",
        "1-123",
        "1234567890-cafe",
        "123-f@bb",
    };
    for (int i = 0; i < sizeof(kBadStrings)/sizeof(char*); i++) {
        revidBuffer rev;
        Assert(!rev.tryParse(slice(kBadStrings[i]), true),
               @"Mistakenly parsed '%s'", kBadStrings[i]);
    }

    // Make sure we don't parse new-style IDs with the old parser:
    revidBuffer rev;
    Assert(!rev.tryParse(slice("17@snej"), false));
}


- (void) test01_Empty {
    VersionedDocument v(*db, @"foo");
    AssertEqual((NSString*)v.docID(), @"foo");
    Assert(v.revID() == NULL);
    AssertEq(v.flags(), 0);
    XCTAssert(v.get(stringToRev(@"1-aaaa")) == NULL);
}


- (void) test02_RevTreeInsert {
    RevTree tree;
    const Revision* rev;
    revidBuffer rev1ID(cbforest::slice("1-aaaa"));
    cbforest::slice rev1Data("body of revision");
    int httpStatus;
    rev = tree.insert(rev1ID, rev1Data, false, false,
                      revid(), false, httpStatus);
    Assert(rev);
    AssertEq(httpStatus, 201);
    Assert(rev->revID == rev1ID);
    Assert(rev->inlineBody() == rev1Data);
    AssertEq(rev->parent(), (Revision*)NULL);
    Assert(!rev->isDeleted());

    revidBuffer rev2ID(cbforest::slice("2-bbbb"));
    cbforest::slice rev2Data("second revision");
    auto rev2 = tree.insert(rev2ID, rev2Data, false, false, rev1ID, false, httpStatus);
    Assert(rev2);
    AssertEq(httpStatus, 201);
    Assert(rev2->revID == rev2ID);
    Assert(rev2->inlineBody() == rev2Data);
    Assert(!rev2->isDeleted());

    tree.sort();
    rev = tree.get(rev1ID);
    rev2 = tree.get(rev2ID);
    Assert(rev);
    Assert(rev2);
    AssertEq(rev2->parent(), rev);
    Assert(rev->parent() == NULL);

    AssertEq(tree.currentRevision(), rev2);
    Assert(!tree.hasConflict());

    tree.sort();
    AssertEq(tree[0], rev2);
    AssertEq(tree[1], rev);
    AssertEq(rev->index(), 1u);
    AssertEq(rev2->index(), 0u);

    alloc_slice ext = tree.encode();

    RevTree tree2(ext, 12, 1234);
}

- (void) test03_AddRevision {
    NSString *revID = @"1-fadebead", *body = @"{\"hello\":true}";
    revidBuffer revIDBuf(revID);
    VersionedDocument v(*db, @"foo");
    int httpStatus;
    v.insert(revIDBuf, nsstring_slice(body), false, false, NULL, false, httpStatus);
    AssertEq(httpStatus, 201);

    const Revision* node = v.get(stringToRev(revID));
    Assert(node);
    Assert(!node->isDeleted());
    Assert(node->isLeaf());
    Assert(node->isActive());
    AssertEq(v.size(), 1u);
    AssertEq(v.currentRevisions().size(), 1u);
    AssertEq(v.currentRevisions()[0], v.currentRevision());
}

- (void) test04_DocType {
    revidBuffer rev1ID(cbforest::slice("1-aaaa"));
    {
        VersionedDocument v(*db, @"foo");

        cbforest::slice rev1Data("body of revision");
        int httpStatus;
        v.insert(rev1ID, rev1Data, true /*deleted*/, false,
                 revid(), false, httpStatus);

        v.setDocType(slice("moose"));
        AssertEq(v.docType(), slice("moose"));
        Transaction t(db);
        v.save(t);
    }
    {
        VersionedDocument v(*db, @"foo");
        AssertEq(v.flags(), VersionedDocument::kDeleted);
        AssertEq(v.revID(), rev1ID);
        AssertEq(v.docType(), slice("moose"));
    }
}

@end
