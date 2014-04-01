//
//  Versions_Tests.m
//  CBForest
//
//  Created by Jens Alfke on 3/28/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import <CBForest/CBForest.h>
#import <CBForest/CBForestPrivate.h>
#import <forestdb.h>


#define kDBPath @"/tmp/forest.db"


@interface Versions_Tests : XCTestCase
@end


@implementation Versions_Tests
{
    CBForestDB* _db;
    CBForestDocument* doc;
    CBForestVersions* vers;
}


- (void) setUp {
    NSError* error;
    if (![[NSFileManager defaultManager] removeItemAtPath: kDBPath error: &error])
        NSLog(@"WARNING: Couldn't delete db file: %@", error);

    _db = [[CBForestDB alloc] initWithFile: kDBPath readOnly: NO error: &error];
    XCTAssert(_db, @"Couldn't open db: %@", error);
    doc = [_db makeDocumentWithID: @"foo"];
    vers = [[CBForestVersions alloc] initWithDocument: doc error: &error];
    XCTAssert(vers, @"Creating CBForestVersions failed: %@", error);
}

- (void) tearDown {
    [_db close];
    _db = nil;
    fdb_shutdown(); // workaround for MB-10674
    [super tearDown];
}


- (void) test00_CompactRevisions {
    NSArray* goodRevs = @[@"92-abf9", @"1-fadebead",
                          @"240-1234567812345678123456781234567812345678"];
    for (NSString* revID in goodRevs) {
        NSData* compact = CompactRevID(revID);
        XCTAssert(compact != nil);
        XCTAssert(compact.length < revID.length);
        NSString* expanded = ExpandRevID(DataToBuf(compact));
        XCTAssertEqualObjects(expanded, revID);
    }

    NSArray* allowableRevs = @[@"1-xxxx", @"299-abcd", @"1-1234567"];
    for (NSString* revID in allowableRevs) {
        NSData* compact = CompactRevID(revID);
        XCTAssert(compact != nil);
        NSString* expanded = ExpandRevID(DataToBuf(compact));
        XCTAssertEqualObjects(expanded, revID);
    }
}


- (void) test01_Empty {
    XCTAssertNil(vers.currentRevisionData);
    XCTAssertEqual(vers.revisionCount, 0);
    XCTAssert(!vers.hasConflicts);
    XCTAssertEqual(vers.currentRevisionIDs.count, 0);
}


- (void) test02_AddRevision {
    NSData* body = [@"{\"hello\":true}" dataUsingEncoding: NSUTF8StringEncoding];
    NSString* revID = @"1-fadebead";
    XCTAssert([vers addRevision: body deletion: NO withID: revID parentID: nil]);
    XCTAssert([vers hasRevision: revID]);
    XCTAssertEqualObjects([vers dataOfRevision: revID], body);
    XCTAssert(![vers isRevisionDeleted: revID]);
    XCTAssertEqual(vers.revisionCount, 1);
    XCTAssertEqualObjects(vers.currentRevisionIDs, @[revID]);
    XCTAssert(!vers.hasConflicts);

    XCTAssertEqual(doc.flags, 0);
    XCTAssertEqualObjects(doc.revID, revID);

    // Save document:
    NSError* error;
    XCTAssert([vers save: &error], @"Vers save failed: %@", error);

    // Reload:
    doc = [_db documentWithID: @"foo" options: 0 error: &error];
    XCTAssert(doc, @"Reloading doc failed: %@", error);
    vers = [[CBForestVersions alloc] initWithDocument: doc error: &error];
    XCTAssert(vers, @"Reloading CBForestVersions failed: %@", error);

    XCTAssertEqual(doc.flags, 0);
    XCTAssertEqualObjects(doc.revID, revID);

    // Test versions again:
    XCTAssert([vers hasRevision: revID]);
    XCTAssertEqualObjects([vers dataOfRevision: revID], body);
    XCTAssert(![vers isRevisionDeleted: revID]);
    XCTAssertEqual(vers.revisionCount, 1);
    XCTAssertEqualObjects(vers.currentRevisionIDs, @[revID]);
    XCTAssert(!vers.hasConflicts);
}


- (void) test03_AddMultipleRevisions {
    NSString* parentID = nil;
    for (int i = 1; i < 100; i++) {
        NSString* bodyStr = [NSString stringWithFormat: @"{\"i\":%d}", i];
        NSString* revID = [NSString stringWithFormat: @"%d-xxxx", i];
        XCTAssert([vers addRevision: [bodyStr dataUsingEncoding: NSUTF8StringEncoding]
                           deletion: NO
                             withID: revID parentID: parentID]);
        parentID = revID;
    }

    // Save document:
    NSError* error;
    XCTAssert([vers save: &error], @"Vers save failed: %@", error);
    NSLog(@"Body size = %llu", doc.bodyLength);

    // Reload:
    doc = [_db documentWithID: @"foo" options: 0 error: &error];
    XCTAssert(doc, @"Reloading doc failed: %@", error);
    vers = [[CBForestVersions alloc] initWithDocument: doc error: &error];
    XCTAssert(vers, @"Reloading CBForestVersions failed: %@", error);

    XCTAssertEqual(doc.flags, 0);
    XCTAssertEqualObjects(doc.revID, parentID);

    // Verify revisions:
    for (int i = 1; i < 100; i++) {
        NSString* bodyStr = [NSString stringWithFormat: @"{\"i\":%d}", i];
        NSString* revID = [NSString stringWithFormat: @"%d-xxxx", i];
        XCTAssertEqualObjects([vers dataOfRevision: revID],
                              [bodyStr dataUsingEncoding: NSUTF8StringEncoding]);
    }

    // Add a conflict:
    XCTAssert([vers addRevision: [@"{\"isConflict\":true}" dataUsingEncoding: NSUTF8StringEncoding]
                       deletion: NO
                         withID: @"51-yyyy" parentID: @"50-xxxx"]);
    XCTAssert(vers.hasConflicts);
    XCTAssertEqual(doc.flags, kCBForestDocConflicted);
    XCTAssertEqualObjects(doc.revID, parentID);
    XCTAssert([vers save: &error], @"Vers save failed: %@", error);
    NSLog(@"Body size = %llu", doc.bodyLength);

    // Reload:
    doc = [_db documentWithID: @"foo" options: 0 error: &error];
    XCTAssert(doc, @"Reloading doc failed: %@", error);
    vers = [[CBForestVersions alloc] initWithDocument: doc error: &error];
    XCTAssert(vers, @"Reloading CBForestVersions failed: %@", error);

    XCTAssertEqualObjects([vers currentRevisionIDs], (@[@"99-xxxx", @"51-yyyy"]));

    // Verify revisions:
    for (int i = 1; i < 100; i++) {
        NSString* bodyStr = [NSString stringWithFormat: @"{\"i\":%d}", i];
        NSString* revID = [NSString stringWithFormat: @"%d-xxxx", i];
        XCTAssertEqualObjects([vers dataOfRevision: revID],
                              [bodyStr dataUsingEncoding: NSUTF8StringEncoding], @"i=%d",i);
    }

    // Delete one branch to resolve the conflict:
    XCTAssert([vers addRevision: nil
                       deletion: YES
                         withID: @"100-zzzz" parentID: @"99-xxxx"]);
    XCTAssert(!vers.hasConflicts);
    XCTAssertEqual(doc.flags, 0);
    XCTAssertEqualObjects(doc.revID, @"51-yyyy");
    XCTAssert([vers save: &error], @"Vers save failed: %@", error);
    NSLog(@"Body size = %llu", doc.bodyLength);
}


@end