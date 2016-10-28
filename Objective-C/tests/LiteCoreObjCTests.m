//
//  LiteCoreObjCTests.m
//  LiteCoreObjCTests
//
//  Created by Jens Alfke on 10/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "LCDatabase.h"
#import "LCDocument.h"
#import "c4Base.h"


#define Assert XCTAssert
#define AssertNotNil XCTAssertNotNil
#define AssertEqual XCTAssertEqual
#define AssertEqualObjects XCTAssertEqualObjects
#define AssertFalse XCTAssertFalse


@interface LiteCoreObjCTests : XCTestCase
@end


@implementation LiteCoreObjCTests
{
    LCDatabase* db;
}


- (void)setUp {
    [super setUp];
    // (Can't use default directory because XCTest doesn't have a bundle ID)
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent: @"LCDatabaseTest"];
    NSError* error;
    Assert([LCDatabase deleteDatabaseAtPath: path error: &error]);
    db = [[LCDatabase alloc] initWithPath: path error: &error];
    AssertNotNil(db, @"Couldn't open db: %@", error);
}


- (void)tearDown {
    NSError *error;
    Assert([db close: &error]);
    db = nil;
    [super tearDown];
}


- (void)test01_NewDoc {
    // Create a new document:
    LCDocument* doc = db[@"doc1"];
    AssertNotNil(doc);
    AssertEqualObjects(doc.documentID, @"doc1");
    AssertEqual(doc.database, db);
    AssertEqual(doc.sequence, 0);
    AssertFalse(doc.exists);
    AssertFalse(doc.isDeleted);
    AssertEqual(doc.properties, nil);
    AssertEqual(doc[@"prop"], nil);
    AssertFalse([doc boolForKey: @"prop"]);
    AssertEqual([doc integerForKey: @"prop"], 0);
    AssertEqual([doc floatForKey: @"prop"], 0.0);
    AssertEqual([doc doubleForKey: @"prop"], 0.0);
    AssertFalse(doc.hasUnsavedChanges);
    AssertEqual(doc.savedProperties, nil);

    // Try and fail to load:
    NSError* error;
    AssertFalse([doc reload: &error]);
    AssertEqualObjects(error.domain, LCErrorDomain);
    AssertEqual(error.code, kC4ErrorNotFound);
}


- (void)test02_SetProperties {
    LCDocument* doc = db[@"doc1"];
    doc[@"type"] = @"demo";
    doc[@"weight"] = @12.5;
    doc[@"tags"] = @[@"useless", @"temporary"];

    Assert(doc.hasUnsavedChanges);
    AssertEqualObjects(doc[@"type"], @"demo");
    AssertEqual([doc doubleForKey: @"weight"], 12.5);
    AssertEqualObjects(doc.properties,
                       (@{@"type": @"demo", @"weight": @12.5, @"tags": @[@"useless", @"temporary"]}));
    [doc revertToSaved];
    Assert(!doc.hasUnsavedChanges);
    AssertEqualObjects(doc[@"type"], nil);
    AssertEqual([doc doubleForKey: @"weight"], 0);
    AssertEqualObjects(doc.properties, nil);
}


- (void)test03_SaveNewDoc {
    LCDocument* doc = db[@"doc1"];
    doc[@"type"] = @"demo";
    doc[@"weight"] = @12.5;
    doc[@"tags"] = @[@"useless", @"temporary"];
    NSError *error;
    Assert([doc save: &error], @"Error saving: %@", error);

    Assert(!doc.hasUnsavedChanges);
    AssertEqualObjects(doc[@"type"], @"demo");
    AssertEqualObjects(doc.properties,
                       (@{@"type": @"demo", @"weight": @12.5, @"tags": @[@"useless", @"temporary"]}));
    AssertEqual(doc.sequence, 1);

    // Note: This only works because LCDocuments aren't being uniqued yet, so this
    // 'doc' is a different object
    LCDocument* firstDoc = doc;
    doc = db[@"doc1"];
    Assert(doc != firstDoc);

    AssertEqual(doc.sequence, 1);
    Assert(!doc.hasUnsavedChanges);
    AssertEqualObjects(doc[@"type"], @"demo");
    AssertEqual([doc doubleForKey: @"weight"], 12.5);
    AssertEqualObjects(doc.properties,
                       (@{@"type": @"demo", @"weight": @12.5, @"tags": @[@"useless", @"temporary"]}));
}

@end
