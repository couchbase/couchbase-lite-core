//
//  Index_Tests.m
//  CBForest
//
//  Created by Jens Alfke on 4/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import <forestdb.h>
#import "CBForestIndex.h"


#define kDBPath @"/tmp/temp.fdbindex"


@interface Index_Tests : XCTestCase

@end

@implementation Index_Tests
{
    CBForestIndex* index;
}


- (void) setUp {
    NSError* error;
    [[NSFileManager defaultManager] removeItemAtPath: kDBPath error: &error];
    index = [[CBForestIndex alloc] initWithFile: kDBPath
                                        options: kCBForestDBCreate
                                         config: nil error: &error];
    XCTAssert(index, @"Couldn't open index: %@", error);
}

- (void) tearDown {
    [index close];
    index = nil;
    [super tearDown];
}


- (void) testBasics {
    NSDictionary* docs = @{
        @"CA": @[@"California", @"San Jose", @"San Francisco", @"Cambria"],
        @"WA": @[@"Washington", @"Seattle", @"Port Townsend", @"Skookumchuk"],
        @"OR": @[@"Oregon", @"Portland", @"Eugene"]};
    for (NSString* docID in docs) {
        NSArray* body = docs[docID];
        BOOL changed = [index updateForDocument: docID atSequence: 1
                         addKeys:^(CBForestIndexEmitBlock emit)
        {
            for (NSUInteger i = 1; i < body.count; i++)
                emit(body[i], body[0]);
        }];
        XCTAssert(changed);
    }

    XCTAssertFalse([index updateForDocument: @"XX"
                                 atSequence: 1
                                    addKeys:^(CBForestIndexEmitBlock emit) {}]);

    NSLog(@"--- First query");
    __block int nRows = 0;
    NSError* error;
    CBForestQueryEnumerator* e = [[CBForestQueryEnumerator alloc] initWithIndex: index
                          startKey: nil startDocID: nil
                            endKey: nil endDocID: nil
                           options: NULL
                             error: &error];
    XCTAssert(e, @"Couldn't create query enumerator: %@", error);
    while (e.nextObject) {
        nRows++;
        XCTAssert([e.value isKindOfClass: [NSString class]], @"Bad value %@", e.valueData);
        NSLog(@"key = %@, value=%@, docID = %@", e.key, e.value, e.docID);
    }
    XCTAssertEqual(nRows, 8);

    [index updateForDocument: @"OR" atSequence: 2
                     addKeys:^(CBForestIndexEmitBlock emit)
     {
         NSArray* body = @[@"Oregon", @"Portland", @"Walla Walla", @"Salem"];
         for (NSUInteger i = 1; i < body.count; i++)
             emit(body[i], body[0]);
     }];

    NSLog(@"--- After updating OR");
    nRows = 0;
    e = [[CBForestQueryEnumerator alloc] initWithIndex: index
                                              startKey: nil startDocID: nil
                                                endKey: nil endDocID: nil
                                               options: NULL
                                                 error: &error];
    XCTAssert(e, @"Couldn't create query enumerator: %@", error);
    while (e.nextObject) {
        nRows++;
        XCTAssert([e.value isKindOfClass: [NSString class]], @"Bad value %@", e.valueData);
        NSLog(@"key = %@, value=%@, docID = %@", e.key, e.value, e.docID);
    }
    XCTAssertEqual(nRows, 9);

    [index updateForDocument: @"CA" atSequence: 3
                     addKeys:^(CBForestIndexEmitBlock emit)
     {
     }];

    NSLog(@"--- After removing CA:");
    nRows = 0;
    e = [[CBForestQueryEnumerator alloc] initWithIndex: index
                                              startKey: nil startDocID: nil
                                                endKey: nil endDocID: nil
                                               options: NULL
                                                 error: &error];
    XCTAssert(e, @"Couldn't create query enumerator: %@", error);
    while (e.nextObject) {
        nRows++;
        XCTAssert([e.value isKindOfClass: [NSString class]], @"Bad value %@", e.valueData);
        NSLog(@"key = %@, value=%@, docID = %@", e.key, e.value, e.docID);
    }
    XCTAssertEqual(nRows, 6);

}


@end
