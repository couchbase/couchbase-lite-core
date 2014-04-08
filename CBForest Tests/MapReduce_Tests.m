//
//  MapReduce_Tests.m
//  CBForest
//
//  Created by Jens Alfke on 4/7/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "CBForestMapReduceIndex.h"
#import "CBForestPrivate.h"


#define kDBPath @"/tmp/temp.fdb"
#define kIndexPath @"/tmp/temp.fdbindex"


@interface MapReduce_Tests : XCTestCase
@end


@implementation MapReduce_Tests
{
    CBForestDB* db;
    CBForestMapReduceIndex* index;
}


- (void) setUp {
    NSError* error;
    [[NSFileManager defaultManager] removeItemAtPath: kDBPath error: &error];
    db = [[CBForestDB alloc] initWithFile: kDBPath readOnly: NO error: &error];
    XCTAssert(db, @"Couldn't open db: %@", error);
    [[NSFileManager defaultManager] removeItemAtPath: kIndexPath error: &error];
    index = [[CBForestMapReduceIndex alloc] initWithFile: kIndexPath readOnly: NO error: &error];
    XCTAssert(index, @"Couldn't open index: %@", error);
    index.sourceDatabase = db;
}

- (void) tearDown {
    [index close];
    index = nil;
    [db close];
    db = nil;
    [super tearDown];
}


- (void) writeJSONBody: (id)body ofDocumentID: (NSString*)docID {
    CBForestDocument* doc = [db makeDocumentWithID: docID];
    NSError* error;
    XCTAssert([doc writeBody: JSONToData(body,NULL) metadata: nil error: &error],
              @"Couldn't save doc '%@': %@", docID, error);
}


- (void) testMapReduce {
    // Populate the database:
    NSDictionary* data = @{
                           @"CA": @{@"name": @"California",
                                    @"cities":@[@"San Jose", @"San Francisco", @"Cambria"]},
                           @"WA": @{@"name": @"Washington",
                                    @"cities": @[@"Seattle", @"Port Townsend", @"Skookumchuk"]},
                           @"OR": @{@"name": @"Oregon",
                                    @"cities": @[@"Portland", @"Eugene"]}};
    for (NSString* docID in data) {
        [self writeJSONBody: data[docID] ofDocumentID: docID];
    }

    // Initialize the index:
    __block int nMapCalls = 0;
    index.map = ^(CBForestDocument* doc, CBForestIndexEmitBlock emit) {
        nMapCalls++;
        NSError* error;
        NSData* rawBody = [doc readBody: &error];
        XCTAssert(rawBody, @"Couldn't read doc body: %@", error);
        NSDictionary* body = DataToJSON(rawBody, NULL);
        for (NSString* city in body[@"cities"])
            emit(city, body[@"name"]);
    };

    NSLog(@"--- Updating index");
    NSError* error;
    nMapCalls = 0;
    XCTAssert([index updateIndex: &error], @"Updating index failed: %@", error);
    XCTAssertEqual(nMapCalls, 3);

    NSLog(@"--- First query");
    __block int nRows = 0;
    BOOL ok = [index queryStartKey: nil endKey: nil options: NULL error: &error
                             block: ^(id key, NSString *docID, NSData *rawValue, BOOL *stop)
    {
        nRows++;
        NSLog(@"key = %@, value=%@, docID = %@", key, rawValue, docID);
    }];
    XCTAssert(ok, @"Query failed: %@", error);
    XCTAssertEqual(nRows, 8);

    NSLog(@"--- Updating OR");
    [self writeJSONBody: @{@"name": @"Oregon",
                           @"cities": @[@"Portland", @"Walla Walla", @"Salem"]}
           ofDocumentID: @"OR"];
    nMapCalls = 0;
    XCTAssert([index updateIndex: &error], @"Updating index failed: %@", error);
    XCTAssertEqual(nMapCalls, 1);

    NSLog(@"--- After updating OR");
    nRows = 0;
    ok = [index queryStartKey: nil endKey: nil options: NULL error: &error
                        block: ^(id key, NSString *docID, NSData *rawValue, BOOL *stop)
          {
              nRows++;
              NSLog(@"key = %@, value=%@, docID = %@", key, rawValue, docID);
          }];
    XCTAssert(ok, @"Query failed: %@", error);
    XCTAssertEqual(nRows, 9);

    [self writeJSONBody: @{@"_deleted": @YES} ofDocumentID: @"CA"];
    nMapCalls = 0;
    XCTAssert([index updateIndex: &error], @"Updating index failed: %@", error);
    XCTAssertEqual(nMapCalls, 1);

    NSLog(@"--- After removing CA:");
    nRows = 0;
    ok = [index queryStartKey: nil endKey: nil options: NULL error: &error
                        block: ^(id key, NSString *docID, NSData *rawValue, BOOL *stop)
          {
              nRows++;
              NSLog(@"key = %@, value=%@, docID = %@", key, rawValue, docID);
          }];
    XCTAssert(ok, @"Query failed: %@", error);
    XCTAssertEqual(nRows, 6);
}

@end
