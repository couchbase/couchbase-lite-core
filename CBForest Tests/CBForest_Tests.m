//
//  CBForest_Tests.m
//  CBForest Tests
//
//  Created by Jens Alfke on 3/27/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import <CBForest/CBForest.h>
#import "CBForestPrivate.h"
#import "forestdb.h"


#define kDBPath @"/tmp/forest.db"


@interface CBForest_Tests : XCTestCase
@end


@implementation CBForest_Tests
{
    CBForestDB* _db;
}


- (void) setUp {
    NSError* error;
    if (![[NSFileManager defaultManager] removeItemAtPath: kDBPath error: &error])
        NSLog(@"WARNING: Couldn't delete db file: %@", error);

    _db = [[CBForestDB alloc] initWithFile: kDBPath readOnly: NO error: &error];
    XCTAssert(_db, @"Couldn't open db: %@", error);
}

- (void) tearDown {
    [_db close];
    _db = nil;
    [super tearDown];
}


- (void) test01_Basics {
    XCTAssertEqualObjects(_db.filename, kDBPath);
    NSError* error;
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);
    XCTAssert([[NSFileManager defaultManager] fileExistsAtPath: kDBPath]);
}

- (void) test02_EmptyDoc {
    CBForestDocument* doc = [_db makeDocumentWithID: @"foo"];
    XCTAssert(doc != nil);
    XCTAssertEqual(doc.db, _db);
    XCTAssertEqualObjects(doc.docID, @"foo");
    XCTAssert(!doc.exists);
}

- (void) logDbInfo {
    CBForestDBInfo info = _db.info;
//    NSLog(@"db docCount=%llu, size=%llu, seq=%llu, headerRevNum=%llu",
//          info.documentCount, info.databaseSize, info.lastSequence, info.headerRevNum);
    NSLog(@"db size=%llu, seq=%llu",
          info.databaseSize, info.lastSequence);
}

- (void) test03_SaveDoc {
    [self logDbInfo];
    NSData* body = [@"Hello ForestDB" dataUsingEncoding: NSUTF8StringEncoding];
    NSData* meta = [@"metametameta" dataUsingEncoding: NSUTF8StringEncoding];

    CBForestDocument* doc = [_db makeDocumentWithID: @"foo"];
    XCTAssertNil(doc.metadata);
    XCTAssertEqual(doc.sequence, kCBForestNoSequence);

    NSError* error;
    XCTAssert([doc writeBody: body metadata: meta error: &error], @"Save failed: %@", error);
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);
    [self logDbInfo];

    XCTAssertEqualObjects([doc readBody: &error], body, @"Read failed, err=%@", error);
    XCTAssertEqualObjects(doc.metadata, meta);

    CBForestDocument* doc2 = [_db documentWithID: @"foo" options: 0 error: &error];
    XCTAssert(doc2, @"documentWithID: failed: %@", error);
    XCTAssertEqualObjects(doc2, doc, @"isEqual: failed");
    XCTAssertEqualObjects(doc2.docID, @"foo");
    XCTAssert(doc2.exists);
    XCTAssertEqualObjects([doc2 readBody: &error], body, @"Read failed, err=%@", error);
    XCTAssertEqualObjects(doc2.metadata, meta);
    XCTAssertEqual(doc2.sequence, 1);

    body = [@"Bye!" dataUsingEncoding: NSUTF8StringEncoding];
    meta = [@"meatmeatmeat" dataUsingEncoding: NSUTF8StringEncoding];
    XCTAssert([doc2 writeBody: body metadata: meta error: &error], @"Save failed: %@", error);
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);
    [self logDbInfo];

    CBForestDocument* doc3 = [_db documentWithID: @"foo" options: 0 error: &error];
    XCTAssert(doc3, @"documentWithID: failed: %@", error);
    XCTAssert(doc3.exists);
    XCTAssertEqual(doc3.sequence, 2);
    XCTAssertEqualObjects([doc3 readBody: &error], body, @"Read failed, err=%@", error);
    XCTAssertEqualObjects(doc3.metadata, meta);

    CBForestDocument* docBySeq = [_db documentWithSequence: 2 options: 0 error: &error];
    XCTAssert(docBySeq, @"documentWithSequence: failed: %@", error);
    XCTAssertEqualObjects(docBySeq.docID, @"foo");
    XCTAssert(docBySeq.exists);
    XCTAssertEqual(docBySeq.sequence, 2);
    XCTAssertEqualObjects([docBySeq readBody: &error], body, @"Read failed, err=%@", error);
    XCTAssertEqualObjects(docBySeq.metadata, meta);

    // Now let's try updating the original doc's metadata:
    XCTAssert([doc reloadMeta: &error], @"refreshMeta failed: %@", error);
    XCTAssertEqualObjects(doc.metadata, meta);

    // ...and body:
    NSData* curBody = [doc readBody: &error];
    XCTAssert(curBody != nil, @"getBody: failed: %@", error);
    XCTAssertEqualObjects(curBody, body);
    XCTAssertEqual(doc.sequence, 2);
}

- (void) test04_EnumerateDocs {
    NSError* error;
    for (int i = 1; i <= 100; i++) {
        NSString* docID = [NSString stringWithFormat: @"doc-%03d", i];
        CBForestDocument* doc = [_db makeDocumentWithID: docID];
        XCTAssert([doc writeBody: [docID dataUsingEncoding: NSUTF8StringEncoding]
                        metadata: nil
                           error: &error], @"Save failed: %@", error);
        XCTAssertEqual(doc.sequence, i);
        XCTAssertEqualObjects([doc readBody: &error],
                              [docID dataUsingEncoding: NSUTF8StringEncoding],
                              @"(i=%d)", i);
    }
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);
    XCTAssert([_db compact: &error], @"Compact failed: %@", error);

    __block int i = 5;
    BOOL ok = [_db enumerateDocsFromID: @"doc-005" toID: @"doc-050" options: 0 error: &error
                             withBlock: ^(CBForestDocument *doc, BOOL *stop)
    {
        XCTAssertEqual(doc.db, _db);
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        XCTAssertEqualObjects(doc.docID, expectedDocID);
        XCTAssertEqual(doc.sequence, i);
        NSError* error;
        XCTAssertEqualObjects([doc readBody: &error],
                              [expectedDocID dataUsingEncoding: NSUTF8StringEncoding],
                              @"(i=%d)", i);
        i++;
    }];
    XCTAssert(ok);
    XCTAssertEqual(i, 51);


    i = 5;
    ok = [_db enumerateDocsFromSequence: 5 toSequence: 50 options: 0 error: &error
                              withBlock: ^(CBForestDocument *doc, BOOL *stop)
    {
        NSLog(@"i=%2d, doc=%@", i, doc);
        XCTAssertEqual(doc.db, _db);
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        XCTAssertEqualObjects(doc.docID, expectedDocID);
        XCTAssertEqual(doc.sequence, i);
        NSError* error;
        XCTAssertEqualObjects([doc readBody: &error],
                              [expectedDocID dataUsingEncoding: NSUTF8StringEncoding],
                              @"(i=%d)", i);
        i++;
    }];
    XCTAssert(ok);
    XCTAssertEqual(i, 51);
}

@end
