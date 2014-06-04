//
//  Index_Test.m
//  CBForest
//
//  Created by Jens Alfke on 5/25/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "testutil.h"
#import "Index.hh"
#import "Collatable.hh"

using namespace forestdb;

#define kDBPath "/tmp/temp.fdbindex"


@interface Index_Test : XCTestCase
@end


@implementation Index_Test
{
    Index* index;
}


- (void) setUp {
    NSError* error;
    [[NSFileManager defaultManager] removeItemAtPath: @"" kDBPath error: &error];
    index = new Index(kDBPath, FDB_OPEN_FLAG_CREATE, Database::defaultConfig());
    Assert(index, @"Couldn't open index: %@", error);
}

- (void) tearDown {
    delete index;
    [super tearDown];
}


- (void) updateDoc: (NSString*)docID body: (NSArray*)body
       transaction: (IndexTransaction&)trans
{
    std::vector<Collatable> keys, values;
    for (NSUInteger i = 1; i < body.count; i++) {
        Collatable key;
        key << [body[i] UTF8String];
        keys.push_back(key);
        Collatable value;
        value << [body[0] UTF8String];
        values.push_back(value);
    }
    bool changed = index->update(trans, docID, 1, keys, values);
    XCTAssert(changed);
}


- (void) testBasics {
    NSDictionary* docs = @{
        @"CA": @[@"California", @"San Jose", @"San Francisco", @"Cambria"],
        @"WA": @[@"Washington", @"Seattle", @"Port Townsend", @"Skookumchuk"],
        @"OR": @[@"Oregon", @"Portland", @"Eugene"]};
    {
        NSLog(@"--- Populate index");
        IndexTransaction trans(index);
        for (NSString* docID in docs)
            [self updateDoc: docID body: docs[docID] transaction: trans];
    }

    NSLog(@"--- First query");
    __block int nRows = 0;
    for (auto e = index->enumerate(forestdb::slice::null, forestdb::slice::null,
                                   forestdb::slice::null, forestdb::slice::null,  NULL); e; ++e) {

        nRows++;
        CollatableReader keyReader(e.key());
        alloc_slice keyStr = keyReader.readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 8);

    {
        IndexTransaction trans(index);
        NSLog(@"--- Updating OR");
        [self updateDoc: @"OR" body: @[@"Oregon", @"Portland", @"Walla Walla", @"Salem"]
            transaction: trans];
    }
    nRows = 0;
    for (auto e = index->enumerate(forestdb::slice::null, forestdb::slice::null,
                                   forestdb::slice::null, forestdb::slice::null,  NULL); e; ++e) {

        nRows++;
        CollatableReader keyReader(e.key());
        alloc_slice keyStr = keyReader.readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 9);

    {
        NSLog(@"--- Removing CA");
        IndexTransaction trans(index);
        [self updateDoc: @"CA" body: @[] transaction: trans];
    }
    nRows = 0;
    for (auto e = index->enumerate(forestdb::slice::null, forestdb::slice::null,
                                   forestdb::slice::null, forestdb::slice::null,  NULL); e; ++e) {

        nRows++;
        CollatableReader keyReader(e.key());
        alloc_slice keyStr = keyReader.readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 6);
}

@end
