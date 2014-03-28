//
//  CBForest_Tests.m
//  CBForest Tests
//
//  Created by Jens Alfke on 3/27/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import <CBForest/CBForest.h>
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

    _db = [[CBForestDB alloc] initWithFile: kDBPath error: &error];
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
    XCTAssertEqualObjects(doc.body, nil);
    XCTAssertEqualObjects(doc.revID, nil);
    XCTAssertEqual(doc.flags, 0);
}

- (void) test03_SaveDoc {
    NSData* body = [@"Hello ForestDB" dataUsingEncoding: NSUTF8StringEncoding];
    NSString* revID = @"1-cafebabe";

    CBForestDocument* doc = [_db makeDocumentWithID: @"foo"];
    XCTAssert(!doc.changed);
    doc.body = body;
    doc.revID = revID;
    XCTAssertEqualObjects(doc.body, body);
    XCTAssertEqualObjects(doc.revID, revID);
    XCTAssertEqual(doc.flags, 0);
    XCTAssertEqual(doc.sequence, kForestDocNoSequence);
    XCTAssert(doc.changed);

    NSError* error;
    XCTAssert([doc saveChanges: &error], @"Save failed: %@", error);
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);

    CBForestDocument* doc2 = [_db documentWithID: @"foo" options: 0 error: &error];
    XCTAssert(doc2, @"documentWithID: failed: %@", error);
    XCTAssert(!doc2.changed);
    XCTAssertEqualObjects(doc2, doc, @"isEqual: failed");
    XCTAssertEqualObjects(doc2.docID, @"foo");
    XCTAssert(doc2.exists);
    XCTAssertEqualObjects(doc2.body, body);
    XCTAssertEqualObjects(doc2.revID, revID);
    XCTAssertEqual(doc.flags, 0);
    XCTAssertEqual(doc2.sequence, 0);

    body = [@"Bye!" dataUsingEncoding: NSUTF8StringEncoding];
    doc.body = body;
    doc.flags = kCBForestDocDeleted;
    XCTAssert(doc.changed);
    XCTAssert([doc saveChanges: &error], @"Save failed: %@", error);
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);

    CBForestDocument* doc3 = [_db documentWithID: @"foo" options: 0 error: &error];
    XCTAssert(doc3, @"documentWithID: failed: %@", error);
    XCTAssertEqualObjects(doc3.body, body);
    XCTAssert(doc3.exists);
    XCTAssertEqual(doc3.sequence, 1);
    XCTAssertEqualObjects(doc3.revID, revID);
    XCTAssertEqual(doc3.flags, kCBForestDocDeleted);

    CBForestDocument* docBySeq = [_db documentWithSequence: 1 options: 0 error: &error];
    XCTAssert(docBySeq, @"documentWithSequence: failed: %@", error);
    XCTAssertEqualObjects(docBySeq.docID, @"foo");
    XCTAssert(docBySeq.exists);
    XCTAssertEqual(docBySeq.sequence, 1);
    XCTAssertEqualObjects(docBySeq.body, body);
    XCTAssertEqualObjects(docBySeq.revID, revID);
    XCTAssertEqual(docBySeq.flags, kCBForestDocDeleted);

    // Now let's try updating the original doc's metadata:
    XCTAssert([doc reloadMeta: &error], @"refreshMeta failed: %@", error);
    XCTAssertEqualObjects(doc.revID, revID);
    XCTAssertEqual(doc.flags, kCBForestDocDeleted);

    // ...and body:
    NSData* curBody = [doc getBody: &error];
    XCTAssert(curBody != nil, @"getBody: failed: %@", error);
    XCTAssertEqualObjects(curBody, body);
    XCTAssertEqual(doc.sequence, 1);
}

- (void) test04_EnumerateDocs {
    NSError* error;
    for (int i = 0; i < 100; i++) {
        NSString* docID = [NSString stringWithFormat: @"doc-%02d", i];
        CBForestDocument* doc = [_db makeDocumentWithID: docID];
        doc.body = [docID dataUsingEncoding: NSUTF8StringEncoding];
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
//      XCTAssertEqual(doc.sequence, i); //Disabled because it fails due to MB-10676
        XCTAssertEqualObjects(doc.body, [expectedDocID dataUsingEncoding: NSUTF8StringEncoding]);
        i++;
    }];
    XCTAssert(ok);
    XCTAssertEqual(i, 51);
}

- (void) test05_MetaOnly {
    NSData* body = [@"Hello ForestDB" dataUsingEncoding: NSUTF8StringEncoding];
    NSString* revID = @"1-cafebabe";

    CBForestDocument* doc = [_db makeDocumentWithID: @"foo"];
    doc.body = body;
    doc.revID = revID;
    NSError* error;
    XCTAssert([doc saveChanges: &error], @"Save failed: %@", error);
    XCTAssert([_db commit: &error], @"Commit failed: %@", error);

    CBForestDocument* doc2 = [_db documentWithID: @"foo" options: kCBForestDBMetaOnly error: &error];
    XCTAssert(!doc2.changed);
    XCTAssertEqualObjects(doc2, doc, @"isEqual: failed");
    XCTAssertEqualObjects(doc2.docID, @"foo");
    XCTAssert(doc2.exists);
    XCTAssert(doc2.body == nil);
    XCTAssertEqual(doc2.bodyLength, body.length);
    XCTAssertEqualObjects(doc2.revID, revID);
    XCTAssertEqual(doc.flags, 0);
    XCTAssertEqual(doc2.sequence, 0);

}

@end
