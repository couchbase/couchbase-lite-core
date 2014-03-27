//
//  CBForest_Tests.m
//  CBForest Tests
//
//  Created by Jens Alfke on 3/27/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import <CBForest/CBForest.h>
#import <CBForest/CBForestDocument.h>


#define kDBPath @"/tmp/forest.db"


@interface CBForest_Tests : XCTestCase
@end


@implementation CBForest_Tests
{
    CBForest* _db;
}


- (void) setUp {
    [[NSFileManager defaultManager] removeItemAtPath: kDBPath error: NULL];

    NSError* error;
    _db = [[CBForest alloc] initWithFile: kDBPath error: &error];
    XCTAssert(_db, @"Couldn't open db: %@", error);
}

- (void) tearDown {
    [_db close];
    [super tearDown];
}


- (void) testBasics {
    XCTAssertEqualObjects(_db.filename, kDBPath);
    NSError* error;
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);
    XCTAssert([[NSFileManager defaultManager] fileExistsAtPath: kDBPath]);
}

- (void) testEmptyDoc {
    CBForestDocument* doc = [_db makeDocumentWithID: @"foo"];
    XCTAssert(doc != nil);
    XCTAssertEqual(doc.db, _db);
    XCTAssertEqualObjects(doc.docID, @"foo");
    XCTAssert(!doc.exists);
    XCTAssertEqualObjects(doc.data, nil);
    XCTAssertEqualObjects(doc.metadata, nil);
}

- (void) testSaveDoc {
    NSData* data = [@"Hello ForestDB" dataUsingEncoding: NSUTF8StringEncoding];
    NSData* metadata = [NSData dataWithBytes: "\0\1\2" length: 3];

    CBForestDocument* doc = [_db makeDocumentWithID: @"foo"];
    doc.data = data;
    doc.metadata = metadata;
    XCTAssertEqualObjects(doc.data, data);
    XCTAssertEqualObjects(doc.metadata, metadata);

    NSError* error;
    XCTAssert([doc saveChanges: &error], @"Save failed: %@", error);
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);

    CBForestDocument* doc2 = [_db documentWithID: @"foo" error: &error];
    XCTAssert(doc2, @"documentWithID: failed: %@", error);
    XCTAssertEqualObjects(doc2, doc, @"isEqual: failed");
    XCTAssertEqualObjects(doc2.docID, @"foo");
    XCTAssert(doc2.exists);
    XCTAssertEqualObjects(doc2.data, data);
    XCTAssertEqualObjects(doc2.metadata, metadata);
    XCTAssertEqual(doc2.sequence, 0);

    data = [@"Bye!" dataUsingEncoding: NSUTF8StringEncoding];
    doc.data = data;
    XCTAssert([doc saveChanges: &error], @"Save failed: %@", error);
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);

    CBForestDocument* doc3 = [_db documentWithID: @"foo" error: &error];
    XCTAssert(doc3, @"documentWithID: failed: %@", error);
    XCTAssertEqualObjects(doc3.data, data);
    XCTAssert(doc3.exists);
    XCTAssertEqual(doc3.sequence, 1);

    CBForestDocument* docBySeq = [_db documentWithSequence: 1 error: &error];
    XCTAssert(docBySeq, @"documentWithSequence: failed: %@", error);
    XCTAssertEqualObjects(docBySeq.docID, @"foo");
    XCTAssert(docBySeq.exists);
    XCTAssertEqual(docBySeq.sequence, 1);
    XCTAssertEqualObjects(docBySeq.data, data);
    XCTAssertEqualObjects(docBySeq.metadata, metadata);
}

@end
