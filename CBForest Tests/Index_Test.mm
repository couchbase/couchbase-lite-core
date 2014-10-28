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
    Index* index;
    uint64_t _rowCount;
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
    bool changed = trans.update(nsstring_slice(docID), 1, keys, values, _rowCount);
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
    for (IndexEnumerator e(*index, Collatable(), forestdb::slice::null,
                                   Collatable(), forestdb::slice::null,
                                   DocEnumerator::Options::kDefault); e; ++e) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 8);
    AssertEq(_rowCount, nRows);

    {
        IndexTransaction trans(index);
        NSLog(@"--- Updating OR");
        [self updateDoc: @"OR" body: @[@"Oregon", @"Portland", @"Walla Walla", @"Salem"]
            transaction: trans];
    }
    nRows = 0;
    for (IndexEnumerator e(*index, Collatable(), forestdb::slice::null,
                                   Collatable(), forestdb::slice::null,
                                   DocEnumerator::Options::kDefault); e; ++e) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 9);
    AssertEq(_rowCount, nRows);

    {
        NSLog(@"--- Removing CA");
        IndexTransaction trans(index);
        [self updateDoc: @"CA" body: @[] transaction: trans];
    }
    nRows = 0;
    for (IndexEnumerator e(*index, Collatable(), forestdb::slice::null,
                           Collatable(), forestdb::slice::null,
                           DocEnumerator::Options::kDefault); e; ++e) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 6);
    AssertEq(_rowCount, nRows);

    NSLog(@"--- Reverse enumeration");
    nRows = 0;
    auto options = DocEnumerator::Options::kDefault;
    options.descending = true;
    for (IndexEnumerator e(*index, Collatable(), forestdb::slice::null,
                           Collatable(), forestdb::slice::null,
                           options); e; ++e) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 6);
    AssertEq(_rowCount, nRows);

    // Enumerate a vector of keys:
    NSLog(@"--- Enumerating a vector of keys");
    std::vector<Collatable> keys;
    keys.push_back(Collatable("Cambria"));
    keys.push_back(Collatable("San Jose"));
    keys.push_back(Collatable("Portland"));
    keys.push_back(Collatable("Skookumchuk"));
    nRows = 0;
    for (IndexEnumerator e(*index, keys, DocEnumerator::Options::kDefault); e; ++e) {
        nRows++;
        alloc_slice keyStr = e.key().readString();
        NSLog(@"key = %.*s, docID = %.*s",
              (int)keyStr.size, keyStr.buf, (int)e.docID().size, e.docID().buf);
    }
    XCTAssertEqual(nRows, 2);
}

- (void) testBlockScopedObjects {
    boolBlock block = scopedEnumerate();
    while (block()) {
        fprintf(stderr, "In while loop...\n");
    }
    fprintf(stderr, "Done!\n");
}

@end
