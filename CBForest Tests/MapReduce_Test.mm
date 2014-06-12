//
//  MapReduce_Test.m
//  CBForest
//
//  Created by Jens Alfke on 5/27/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "testutil.h"
#import "MapReduceIndex.hh"
#import "Collatable.hh"

using namespace forestdb;

extern "C" {
    NSData* JSONToData(id obj, NSError** outError) ;
}

static inline id DataToJSON(NSData* data, NSError** outError) {
    return [NSJSONSerialization JSONObjectWithData: data
                                           options: NSJSONReadingAllowFragments
                                             error: NULL];
}

static Collatable StringToCollatable(NSString* str) {
    Collatable c;
    c << str;
    return c;
}


#define kDBPath "/tmp/temp.fdb"
#define kIndexPath "/tmp/temp.fdbindex"


class TestMapFn : public MapFn {
public:
    static int numMapCalls;
    virtual void operator() (const Document& doc, EmitFn& emit) {
        ++numMapCalls;
        NSDictionary* body = DataToJSON((NSData*)doc.body(), NULL);
        for (NSString* city in body[@"cities"])
            emit(StringToCollatable(city), StringToCollatable(body[@"name"]));
    }
};

int TestMapFn::numMapCalls;


@interface MapReduce_Test : XCTestCase
@end

@implementation MapReduce_Test
{
    Database* db;
    MapReduceIndex* index;
}

- (void) setUp {
    NSError* error;
    [[NSFileManager defaultManager] removeItemAtPath: @"" kDBPath error: &error];
    db = new Database(kDBPath, FDB_OPEN_FLAG_CREATE, Database::defaultConfig());
    Assert(db, @"Couldn't open db: %@", error);
    [[NSFileManager defaultManager] removeItemAtPath: @"" kIndexPath error: &error];
    index = new MapReduceIndex(kIndexPath, FDB_OPEN_FLAG_CREATE, MapReduceIndex::defaultConfig(), db);
    Assert(index, @"Couldn't open index: %@", error);
}

- (void)tearDown
{
    delete index;
    delete db;
    [super tearDown];
}


- (void) queryExpectingKeys: (NSArray*)expectedKeys {
    index->updateIndex();
    int nRows = 0;
    for (auto e = index->enumerate(Collatable(), forestdb::slice::null,
                                   Collatable(), forestdb::slice::null,  NULL); e; ++e) {

        CollatableReader keyReader(e.key());
        alloc_slice keyStr = keyReader.readString();
        NSString* key = (NSString*)keyStr;
        NSLog(@"key = %@, docID = %.*s",
              key, (int)e.docID().size, e.docID().buf);
        AssertEqual(key, expectedKeys[nRows++]);
    }
    XCTAssertEqual(nRows, expectedKeys.count);
}


- (void) testMapReduce {
    {
        // Populate the database:
        NSDictionary* data = @{
               @"CA": @{@"name": @"California",
                        @"cities":@[@"San Jose", @"San Francisco", @"Cambria"]},
               @"WA": @{@"name": @"Washington",
                        @"cities": @[@"Seattle", @"Port Townsend", @"Skookumchuk"]},
               @"OR": @{@"name": @"Oregon",
                        @"cities": @[@"Portland", @"Eugene"]}};
        Transaction trans(db);
        for (NSString* docID in data) {
            trans.set(docID, forestdb::slice::null, JSONToData(data[docID],NULL));
        }
    }

    index->setup(0, new TestMapFn, "1");

    NSLog(@"--- First query");
    TestMapFn::numMapCalls = 0;
    [self queryExpectingKeys: @[@"Cambria", @"Eugene", @"Port Townsend", @"Portland",
                                @"San Francisco", @"San Jose", @"Seattle", @"Skookumchuk"]];
    AssertEq(TestMapFn::numMapCalls, 3);

    NSLog(@"--- Updating OR");
    {
        Transaction trans(db);
        NSDictionary* body = @{@"name": @"Oregon",
                               @"cities": @[@"Portland", @"Walla Walla", @"Salem"]};
        trans.set(@"OR", forestdb::slice::null, JSONToData(body,NULL));
    }
    [self queryExpectingKeys: @[@"Cambria", @"Port Townsend", @"Portland", @"Salem",
                                @"San Francisco", @"San Jose", @"Seattle", @"Skookumchuk",
                                @"Walla Walla"]];
    AssertEq(TestMapFn::numMapCalls, 4);

    NSLog(@"--- Deleting CA");
    {
        Transaction trans(db);
        trans.del(@"CA");
    }
    [self queryExpectingKeys: @[@"Port Townsend", @"Portland", @"Salem",
                                @"Seattle", @"Skookumchuk", @"Walla Walla"]];
    AssertEq(TestMapFn::numMapCalls, 4);
}

@end
