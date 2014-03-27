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
#import "forestdb.h"


#define kDBPath @"/tmp/forest.db"


@interface CBForest_Tests : XCTestCase
@end


@implementation CBForest_Tests
{
    CBForest* _db;
}


- (void) setUp {
    NSLog(@" ---- setup ----");
    NSError* error;
    if (![[NSFileManager defaultManager] removeItemAtPath: kDBPath error: &error])
        NSLog(@"WARNING: Couldn't delete db file: %@", error);

    _db = [[CBForest alloc] initWithFile: kDBPath error: &error];
    XCTAssert(_db, @"Couldn't open db: %@", error);
}

- (void) tearDown {
    [_db close];
    _db = nil;
    fdb_shutdown(); // workaround for MB-10674
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
    XCTAssertEqualObjects(doc.data, nil);
    XCTAssertEqualObjects(doc.metadata, nil);
}

- (void) test03_SaveDoc {
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

- (void) test04_EnumerateDocs {
    NSError* error;
    for (int i = 0; i < 100; i++) {
        NSString* docID = [NSString stringWithFormat: @"doc-%02d", i];
        CBForestDocument* doc = [_db makeDocumentWithID: docID];
        doc.data = [docID dataUsingEncoding: NSUTF8StringEncoding];
        XCTAssert([doc saveChanges: &error], @"save failed: %@", error);
        XCTAssertEqual(doc.sequence, i);
    }
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);

    __block int i = 5;
    BOOL ok = [_db enumerateDocsFromID: @"doc-05" toID: @"doc-50" options: 0 error: &error
                             withBlock: ^(CBForestDocument *doc, BOOL *stop)
    {
        XCTAssertEqual(doc.db, _db);
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%02d", i];
        XCTAssertEqualObjects(doc.docID, expectedDocID);
//        XCTAssertEqual(doc.sequence, i); //Disabled because it fails due to MB-10676
        XCTAssertEqualObjects(doc.data, [expectedDocID dataUsingEncoding: NSUTF8StringEncoding]);
        i++;
    }];
    XCTAssert(ok);
    XCTAssertEqual(i, 51);
}

@end
