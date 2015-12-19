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

using namespace cbforest;


class Scoped {
public:
    int n;
    Scoped(int initialN)
    :n(initialN)
    {
        fprintf(stderr, " Scoped %p (%d)\n", this, n);
    }
    Scoped(const Scoped& s)
    :n(s.n)
    {
        fprintf(stderr, " Scoped %p (%d) copy ctor\n", this, n);
    }
    Scoped(Scoped&& s)
    :n(s.n)
    {
        s.n = -999;
        fprintf(stderr, " Scoped %p (%d) move ctor\n", this, n);
    }
    ~Scoped() {
        fprintf(stderr, "~Scoped %p\n", this);
    }
};

typedef bool (^boolBlock)();

static boolBlock scopedEnumerate() {
    __block Scoped s(5);
    fprintf(stderr, "In scopedEnumerate, &s = %p; s.n=%d\n", &s, s.n);
    boolBlock block = ^bool{
        fprintf(stderr, "Called enumerate block; n=%d\n", s.n);
        return s.n-- > 0;
    };
    fprintf(stderr, "At end of scopedEnumerate, &s = %p; s.n=%d\n", &s, s.n);
    return block;
}


@interface Index_Test : XCTestCase
@end


@implementation Index_Test
{
    std::string dbPath;
    Database* database;
    Index* index;
    uint64_t _rowCount;
}

+ (void) initialize {
    if (self == [Index_Test class]) {
        LogLevel = kWarning;
    }
}

- (void) setUp {
    [super setUp];
    CreateTestDir();
    dbPath = PathForDatabaseNamed(@"forest_temp.fdb");
    database = new Database(dbPath, TestDBConfig());
    index = new Index(database, "index");
}

- (void) tearDown {
    delete index;
    delete database;
    [super tearDown];
}


- (void) updateDoc: (NSString*)docID body: (NSArray*)body
            writer: (IndexWriter&)writer
{
    std::vector<Collatable> keys;
    std::vector<alloc_slice> values;
    for (NSUInteger i = 1; i < body.count; i++) {
        CollatableBuilder key;
        key << [body[i] UTF8String];
        keys.push_back(key);
        values.push_back(alloc_slice(nsstring_slice(body[0])));
    }
    bool changed = writer.update(nsstring_slice(docID), 1, keys, values, _rowCount);
    XCTAssert(changed);
}


- (int) doQuery {
    unsigned nRows = 0;
    for (IndexEnumerator e(index, Collatable(), cbforest::slice::null,
                           Collatable(), cbforest::slice::null,
                           DocEnumerator::Options::kDefault); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        slice valueStr = e.value();
        NSLog(@"key = %.*s, value = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf,
              (int)valueStr.size, valueStr.buf,
              (int)e.docID().size, e.docID().buf);
    }
    AssertEq(nRows, _rowCount);
    return nRows;
}


- (void) testBasics {
    //LogLevel = kDebug;
    NSDictionary* docs = @{
        @"CA": @[@"California", @"San Jose", @"San Francisco", @"Cambria"],
        @"WA": @[@"Washington", @"Seattle", @"Port Townsend", @"Skookumchuk"],
        @"OR": @[@"Oregon", @"Portland", @"Eugene"]};
    {
        NSLog(@"--- Populate index");
        Transaction trans(database);
        IndexWriter writer(index, trans);
        for (NSString* docID in docs)
            [self updateDoc: docID body: docs[docID] writer: writer];
    }

    NSLog(@"--- First query");
    XCTAssertEqual([self doQuery], 8);

    {
        Transaction trans(database);
        IndexWriter writer(index, trans);
        NSLog(@"--- Updating OR");
        [self updateDoc: @"OR" body: @[@"Oregon", @"Portland", @"Walla Walla", @"Salem"]
                 writer: writer];
    }
    XCTAssertEqual([self doQuery], 9);

    {
        NSLog(@"--- Removing CA");
        Transaction trans(database);
        IndexWriter writer(index, trans);
        [self updateDoc: @"CA" body: @[] writer: writer];
    }
    XCTAssertEqual([self doQuery], 6);

    NSLog(@"--- Reverse enumeration");
    unsigned nRows = 0;
    auto options = DocEnumerator::Options::kDefault;
    options.descending = true;
    for (IndexEnumerator e(index, Collatable(), cbforest::slice::null,
                           Collatable(), cbforest::slice::null,
                           options); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 6u);
    AssertEq(_rowCount, nRows);

    // Enumerate a vector of keys:
    NSLog(@"--- Enumerating a vector of keys");
    std::vector<KeyRange> keys;
    keys.push_back((Collatable)CollatableBuilder("Cambria"));
    keys.push_back((Collatable)CollatableBuilder("San Jose"));
    keys.push_back((Collatable)CollatableBuilder("Portland"));
    keys.push_back((Collatable)CollatableBuilder("Skookumchuk"));
    nRows = 0;
    for (IndexEnumerator e(index, keys, DocEnumerator::Options::kDefault); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 2u);

    // Enumerate a vector of key ranges:
    NSLog(@"--- Enumerating a vector of key ranges");
    std::vector<KeyRange> ranges;
    ranges.push_back(KeyRange(CollatableBuilder("Port"), CollatableBuilder("Port\uFFFE")));
    ranges.push_back(KeyRange(CollatableBuilder("Vernon"), CollatableBuilder("Ypsilanti")));
    nRows = 0;
    for (IndexEnumerator e(index, ranges, DocEnumerator::Options::kDefault); e.next(); ) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 3u);
}

- (void) testDuplicateKeys {
    NSLog(@"--- Populate index");
    {
        Transaction trans(database);
        IndexWriter writer(index, trans);
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        CollatableBuilder key("Schlage");
        keys.push_back(key);
        values.push_back(alloc_slice("purple"));
        keys.push_back(key);
        values.push_back(alloc_slice("red"));
        bool changed = writer.update(slice("doc1"), 1, keys, values, _rowCount);
        Assert(changed);
        AssertEq(_rowCount, 2u);
    }
    NSLog(@"--- First query");
    XCTAssertEqual([self doQuery], 2);
    {
        Transaction trans(database);
        IndexWriter writer(index, trans);
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        CollatableBuilder key("Schlage");
        keys.push_back(key);
        values.push_back(alloc_slice("purple"));
        keys.push_back(key);
        values.push_back(alloc_slice("crimson"));
        keys.push_back(CollatableBuilder("Master"));
        values.push_back(alloc_slice("gray"));
        bool changed = writer.update(slice("doc1"), 2, keys, values, _rowCount);
        Assert(changed);
        AssertEq(_rowCount, 3u);
    }
    NSLog(@"--- Second query");
    XCTAssertEqual([self doQuery], 3);
}

- (void) testBlockScopedObjects {
    boolBlock block = scopedEnumerate();
    while (block()) {
        fprintf(stderr, "In while loop...\n");
    }
    fprintf(stderr, "Done!\n");
}

@end
