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


static NSData* toData(NSString* str) {
    return [str dataUsingEncoding: NSUTF8StringEncoding];
}


@interface CBForest_Tests : XCTestCase
@end


@implementation CBForest_Tests
{
    CBForestDB* _db;
}


- (void) setUp {
    NSError* error;
    [[NSFileManager defaultManager] removeItemAtPath: kDBPath error: &error];
    _db = [[CBForestDB alloc] initWithFile: kDBPath options: kCBForestDBCreate config: nil error: &error];
    XCTAssert(_db, @"Couldn't open db: %@", error);
}

- (void) tearDown {
    [_db close];
    _db = nil;
    [super tearDown];
}


- (void) test01_Basics {
    XCTAssertEqualObjects(_db.filename, kDBPath);
    BOOL ok = [_db inTransaction: ^BOOL{
        return YES;
    }];
    XCTAssert(ok);
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
    NSLog(@"db docCount=%llu, size=%llu, filesize=%llu, seq=%llu",
          info.documentCount, info.dataSize, info.fileSize, info.lastSequence);
}

- (void) test03_SaveDoc {
    [self logDbInfo];
    NSData* body = toData(@"Hello ForestDB");
    NSData* meta = toData(@"metametameta");

    CBForestDocument* doc = [_db makeDocumentWithID: @"foo"];
    XCTAssertNil(doc.metadata);
    XCTAssertEqual(doc.sequence, kCBForestNoSequence);

    NSError* error;
    XCTAssert([doc writeBody: body metadata: meta error: &error], @"Save failed: %@", error);
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

    body = toData(@"Bye!");
    meta = toData(@"meatmeatmeat");
    XCTAssert([doc2 writeBody: body metadata: meta error: &error], @"Save failed: %@", error);
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
    XCTAssert([doc reload: kCBForestDBMetaOnly error: &error], @"refreshMeta failed: %@", error);
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
        XCTAssert([doc writeBody: toData(docID)
                        metadata: nil
                           error: &error], @"Save failed: %@", error);
        XCTAssertEqual(doc.sequence, i);
        XCTAssertEqualObjects([doc readBody: &error],
                              toData(docID),
                              @"(i=%d)", i);
    }
    XCTAssert([_db compact: &error], @"Compact failed: %@", error);

    __block int i = 5;
    NSEnumerator* e = [_db enumerateDocsFromID: @"doc-005" toID: @"doc-050" options: 0 error: &error ];
    XCTAssert(e);
    for (CBForestDocument* doc in e) {
        XCTAssertEqual(doc.db, _db);
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        XCTAssertEqualObjects(doc.docID, expectedDocID);
        XCTAssertEqual(doc.sequence, i);
        NSError* innerError;
        XCTAssertEqualObjects([doc readBody: &innerError],
                              toData(expectedDocID),
                              @"(i=%d)", i);
        i++;
    }
    XCTAssertEqual(i, 51);


    i = 5;
    e = [_db enumerateDocsFromSequence: 5 toSequence: 50 options: 0 error: &error];
    XCTAssert(e);
    for (CBForestDocument* doc in e) {
        NSLog(@"i=%2d, doc=%@", i, doc);
        XCTAssertEqual(doc.db, _db);
        NSString* expectedDocID = [NSString stringWithFormat: @"doc-%03d", i];
        XCTAssertEqualObjects(doc.docID, expectedDocID);
        XCTAssertEqual(doc.sequence, i);
        NSError* innerError;
        XCTAssertEqualObjects([doc readBody: &innerError],
                              toData(expectedDocID),
                              @"(i=%d)", i);
        i++;
    }
    XCTAssertEqual(i, 51);
}

- (void) test05_SnapshotAndRollback {
    // Make some changes:
    XCTAssert([_db setValue: toData(@"value1") meta: nil forKey: toData(@"key1") error: NULL]);
    CBForestSequence sequenceBefore = _db.info.lastSequence;
    XCTAssert([_db setValue: toData(@"OOPSIE") meta: nil forKey: toData(@"key1") error: NULL]);
    XCTAssert([_db setValue: toData(@"VALOO") meta: nil forKey: toData(@"KII") error: NULL]);
    CBForestSequence sequenceAfter = _db.info.lastSequence;
    XCTAssert(sequenceAfter > sequenceBefore);

    // Make a snapshot from right after the first change:
    NSError* error;
    CBForestDB* snapshot = [_db openSnapshotAtSequence: sequenceBefore error: &error];
    XCTAssert(snapshot, @"Couldn't open snapshot: %@", error);

    NSData* value;
    XCTAssert([snapshot getValue: &value meta: NULL forKey: toData(@"key1") error: NULL]);
    XCTAssertEqualObjects(value, toData(@"value1"));
    XCTAssert([snapshot getValue: &value meta: NULL forKey: toData(@"KII") error: NULL]);
    XCTAssertNil(value);
    XCTAssertEqual(snapshot.info.lastSequence, sequenceBefore);
    [snapshot close];

    XCTAssert([_db getValue: &value meta: NULL forKey: toData(@"key1") error: NULL]);
    XCTAssertEqualObjects(value, toData(@"OOPSIE"));
    XCTAssert([_db getValue: &value meta: NULL forKey: toData(@"KII") error: NULL]);
    XCTAssertEqualObjects(value, toData(@"VALOO"));
    XCTAssertEqual(_db.info.lastSequence, sequenceAfter);

    // Roll-back the main handle to right after the first change:
    XCTAssert([_db rollbackToSequence: sequenceBefore error: &error]);

    XCTAssert([_db getValue: &value meta: NULL forKey: toData(@"key1") error: NULL]);
    XCTAssertEqualObjects(value, toData(@"value1"));
    XCTAssert([_db getValue: &value meta: NULL forKey: toData(@"KII") error: NULL]);
    XCTAssertNil(value);
    XCTAssertEqual(_db.info.lastSequence, sequenceBefore);
}

- (void) test06_BenchmarkSnapshot {
    XCTAssert([_db setValue: toData(@"value1") meta: nil forKey: toData(@"key1") error: NULL]);
    CBForestSequence sequence = _db.info.lastSequence;

    const int kIterations = 10000;
    CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
    for (int i = 0; i < kIterations; i++) {
        @autoreleasepool {
            CBForestDB* snapshot = [_db openSnapshotAtSequence: sequence error: NULL];
            [snapshot close];
        }
    }
    NSLog(@"Creating a snapshot took %g µsec", (CFAbsoluteTimeGetCurrent()-start)*1.0e6/kIterations);
}

- (void) test07_AbortTransaction {
    // Initial document:
    XCTAssert([_db setValue: toData(@"value1") meta: nil forKey: toData(@"key1") error: NULL]);
    CBForestSequence sequenceBefore = _db.info.lastSequence;

    // Make changes in a transaction:
    [_db beginTransaction];
    XCTAssert([_db setValue: toData(@"OOPSIE") meta: nil forKey: toData(@"key1") error: NULL]);
    XCTAssert([_db setValue: toData(@"VALOO") meta: nil forKey: toData(@"KII") error: NULL]);

    // Abort the transaction:
    [_db failTransaction];
    NSError* error;
    XCTAssertFalse([_db endTransaction: &error]);
    XCTAssertEqual(error.code, kCBForestErrorTransactionAborted);
    XCTAssertEqual(_db.info.lastSequence, sequenceBefore);

    // Make sure the changes were rolled back:
    NSData* value;
    XCTAssert([_db getValue: &value meta: NULL forKey: toData(@"key1") error: NULL]);
    XCTAssertEqualObjects(value, toData(@"value1"));
}

- (void) test07_AsyncWriteError {
    // Initial document:
    XCTAssert([_db setValue: toData(@"value1") meta: nil forKey: toData(@"key1") error: NULL]);
    CBForestSequence sequenceBefore = _db.info.lastSequence;

    // Make changes in a transaction:
    [_db beginTransaction];
    XCTAssert([_db setValue: toData(@"OOPSIE") meta: nil forKey: toData(@"key1") error: NULL]);
    [_db asyncSetValue: toData(@"VALOO") meta: nil forKey: nil
            onComplete: nil]; // illegal

    // End the transaction:
    NSError* error;
    XCTAssertFalse([_db endTransaction: &error]);
    XCTAssertEqual(error.code, kCBForestErrorInvalidArgs);
    XCTAssertEqual(_db.info.lastSequence, sequenceBefore);

    // Make sure the changes were rolled back:
    NSData* value;
    XCTAssert([_db getValue: &value meta: NULL forKey: toData(@"key1") error: NULL]);
    XCTAssertEqualObjects(value, toData(@"value1"));
}

/*
- (void) test05_Queue {
    CBForestQueue* queue = [[CBForestQueue alloc] initWithCapacity: 4];
    dispatch_queue_t dq = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    const int participants = 20, iterations = 1000;
    __block int32_t readCount=0, writeCount=0, failedWriteCount=0;
    dispatch_apply(participants, dq, ^(size_t i) {
        if (i % 2) {
            NSLog(@"Writer #%d started", (int)i);
            for (int j = 0; j < iterations; j++) {
                usleep(random() % 100);
                NSString* value = [NSString stringWithFormat: @"%03d-%03d", (int)i, j];
                if ([queue push: value])
                    OSAtomicIncrement32(&writeCount);
                else {
                    OSAtomicIncrement32(&failedWriteCount);
                    //NSLog(@"Writer #%d: write #%d failed", (int)i, j);
                }
            }
            NSLog(@"Writer #%d finished", (int)i);
            if (i == participants-1) {
                NSLog(@"Closing queue!");
                [queue close];
            }
        } else {
            int j = 0;
            id value;
            do {
                usleep(random() % 100);
                value = [queue pop];
                //NSLog(@"Reader #%d iter #%d Popped: %@", (int)i, j, value);
                if (value)
                    OSAtomicIncrement32(&readCount);
                j++;
            } while (value);
            NSLog(@"Reader #%d stopped", (int)i/2);
        }
    });
    XCTAssertEqual(readCount, participants/2*iterations);
    XCTAssertEqual(writeCount+failedWriteCount, participants/2*iterations);
}
*/
@end
