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
#import "DocEnumerator.hh"
#import "Collatable.hh"

using namespace cbforest;

static NSData* JSONToData(id obj, NSError** outError) {
    return [NSJSONSerialization dataWithJSONObject: obj options: 0 error: outError];
}

static inline id DataToJSON(NSData* data, NSError** outError) {
    return [NSJSONSerialization JSONObjectWithData: data
                                           options: NSJSONReadingAllowFragments
                                             error: NULL];
}

static CollatableBuilder StringToCollatable(NSString* str) {
    CollatableBuilder c;
    c << str;
    return c;
}


static int numMapCalls;

static void updateIndex(Database *indexDB, MapReduceIndex* index) {
    MapReduceIndexer indexer;
    indexer.addIndex(index);
    auto seq = indexer.startingSequence();
    numMapCalls = 0;
    NSLog(@"Updating index from sequence=%llu...", seq);

    auto options = DocEnumerator::Options::kDefault;
    options.includeDeleted = true;
    DocEnumerator e(index->sourceStore(), seq, UINT64_MAX, options);
    while (e.next()) {
        auto &doc = e.doc();
        NSLog(@"    enumerating seq %llu: '%.*s' (del=%d)",
              doc.sequence(), (int)doc.key().size, doc.key().buf, doc.deleted());
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        if (!doc.deleted()) {
            // Here's the pseudo map function:
            ++numMapCalls;
            NSDictionary* body = DataToJSON(doc.body().copiedNSData(), NULL);
            for (NSString* city in body[@"cities"]) {
                keys.push_back(StringToCollatable(city));
                values.push_back(StringToCollatable(body[@"name"]));
            }
        }
        indexer.emitDocIntoView(doc.key(), doc.sequence(), 0, keys, values);
    }
    indexer.finished();
    NSLog(@"...done updating index (%d map calls)", numMapCalls);
}



@interface MapReduce_Test : XCTestCase
@end

@implementation MapReduce_Test
{
    std::string dbPath;
    Database* db;
    MapReduceIndex* index;
}

+ (void) initialize {
    if (self == [MapReduce_Test class]) {
        LogLevel = kWarning;
    }
}

- (void) setUp {
    [super setUp];
    CreateTestDir();
    dbPath = PathForDatabaseNamed(@"forest_temp.fdb");
    db = new Database(dbPath, TestDBConfig());
    index = new MapReduceIndex(db, "index", db);
    Assert(index, @"Couldn't open index");
}

- (void)tearDown
{
    delete index;
    delete db;
    [super tearDown];
}


- (void) queryExpectingKeys: (NSArray*)expectedKeys {
    updateIndex(db, index);

    unsigned nRows = 0;
    for (IndexEnumerator e(index, Collatable(), cbforest::slice::null,
                           Collatable(), cbforest::slice::null,
                           DocEnumerator::Options::kDefault); e.next(); ) {
        CollatableReader keyReader(e.key());
        alloc_slice keyStr = keyReader.readString();
        NSString* key = (NSString*)keyStr;
        NSLog(@"key = %@, docID = %.*s",
              key, (int)e.docID().size, e.docID().buf);
        AssertEqual(key, expectedKeys[nRows++]);
    }
    XCTAssertEqual(nRows, expectedKeys.count);
    XCTAssertEqual(index->rowCount(), nRows);
}


- (void) createDocsAndIndex {
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
            trans.set(nsstring_slice(docID), cbforest::slice::null, JSONToData(data[docID],NULL));
        }
    }

    index->setup(0, "1");
}


- (void) testMapReduce {
    [self createDocsAndIndex];

    NSLog(@"--- First query");
    [self queryExpectingKeys: @[@"Cambria", @"Eugene", @"Port Townsend", @"Portland",
                                @"San Francisco", @"San Jose", @"Seattle", @"Skookumchuk"]];
    AssertEq(numMapCalls, 3);

    NSLog(@"--- Updating OR");
    NSDictionary* body = @{@"name": @"Oregon",
                           @"cities": @[@"Portland", @"Walla Walla", @"Salem"]};
    Transaction(db).set(nsstring_slice(@"OR"), cbforest::slice::null, JSONToData(body,NULL));
    [self queryExpectingKeys: @[@"Cambria", @"Port Townsend", @"Portland", @"Salem",
                                @"San Francisco", @"San Jose", @"Seattle", @"Skookumchuk",
                                @"Walla Walla"]];
    AssertEq(numMapCalls, 1);

    // After deleting a doc, updating the index can be done incrementally because the deleted doc
    // will appear in the by-sequence iteration, so the indexer can remove its rows.
    NSLog(@"--- Deleting CA");
    Transaction(db).del(nsstring_slice(@"CA"));
    [self queryExpectingKeys: @[@"Port Townsend", @"Portland", @"Salem",
                                @"Seattle", @"Skookumchuk", @"Walla Walla"]];
    AssertEq(numMapCalls, 0);

    NSLog(@"--- Updating version");
    index->setup(0, "2");
    [self queryExpectingKeys: @[@"Port Townsend", @"Portland", @"Salem",
                                @"Seattle", @"Skookumchuk", @"Walla Walla"]];
    AssertEq(numMapCalls, 2);

    // Deletion followed by compaction will purge the deleted docs, so incremental indexing no
    // longer works. The indexer should detect this and rebuild from scratch.
    NSLog(@"--- Deleting OR");
    Transaction(db).del(nsstring_slice(@"OR"));
    NSLog(@"--- Compacting db");
    db->compact();

    [self queryExpectingKeys: @[@"Port Townsend", @"Seattle", @"Skookumchuk"]];
    AssertEq(numMapCalls, 1);
}

- (void) testReopen {
    [self createDocsAndIndex];
    updateIndex(db, index);
    sequence lastIndexed = index->lastSequenceIndexed();
    sequence lastChangedAt = index->lastSequenceChangedAt();
    Assert(lastChangedAt > 0);
    Assert(lastIndexed >= lastChangedAt);

    delete index;
    index = NULL;

    index = new MapReduceIndex(db, "index", db);
    Assert(index, @"Couldn't reopen index");

    index->setup(0, "1");
    AssertEq(index->lastSequenceIndexed(), lastIndexed);
    AssertEq(index->lastSequenceChangedAt(), lastChangedAt);
}

@end
