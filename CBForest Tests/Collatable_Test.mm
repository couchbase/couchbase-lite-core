//
//  Collatable_Test.mm
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "Collatable.hh"
#import "testutil.h"
#import <Security/Security.h>

using namespace forestdb;

@interface Collatable_Test : XCTestCase
@end

@implementation Collatable_Test

template <typename T>
static int sgn(T n) {return n<0 ? -1 : (n>0 ? 1 : 0);}

template <typename T1, typename T2>
static int compareCollated(T1 obj1, T2 obj2) {
    Collatable c1, c2;
    c1 << obj1;
    c2 << obj2;
    return sgn(forestdb::slice(c1).compare(forestdb::slice(c2)));
}

static uint64_t randn(uint64_t limit) {
    uint64_t n;
    SecRandomCopyBytes(kSecRandomDefault, 8, (uint8_t*)&n);
    return n % limit;
}

- (void) compareNumber: (int64_t)n1 with: (int64_t)n2 {
    XCTAssertEqual(compareCollated(n1, n2), sgn((double)n1 - (double)n2),
                   @"Failed collation of %lld with %lld",
                   n1, n2);
}


- (void) testScalars {
    AssertEq(compareCollated(true, false), 1);
    AssertEq(compareCollated(true, false), 1);
    AssertEq(compareCollated(true, 17), -1);
    AssertEq(compareCollated(1, 1), 0);
    AssertEq(compareCollated(123, 1), 1);
    AssertEq(compareCollated(0x100, 0xFF), 1);
    AssertEq(compareCollated(0x1234, 0x12), 1);
    AssertEq(compareCollated(0x1234, 0x13), 1);
    AssertEq(compareCollated((INT64_MAX), (INT32_MAX)), 1);

    AssertEq(compareCollated((-1), (0)), -1);
    AssertEq(compareCollated((-1), (1)), -1);
    AssertEq(compareCollated((-123), (-7)), -1);
}

- (void) testRandomNumbers {
    self.continueAfterFailure = NO;
    for (int i=0; i< 10000; i++) {
        int64_t n1, n2;
        n1 = (int64_t)randn(UINT64_MAX) >> randn(63);
        n2 = (int64_t)randn(UINT64_MAX) >> randn(63);
        [self compareNumber: n1 with: n2];
    }
}

- (void) testStrings {
    AssertEq(compareCollated((std::string)"", 7), 1);
    AssertEq(compareCollated((std::string)"", (std::string)""), 0);
    AssertEq(compareCollated((std::string)"", true), 1);
    AssertEq(compareCollated((std::string)"", (std::string)" "), -1);
    AssertEq(compareCollated((std::string)"~", (std::string)"a"), -1);
    AssertEq(compareCollated((std::string)"A", (std::string)"a"), 1);
    AssertEq(compareCollated((std::string)"\n", (std::string)" "), -1);
    AssertEq(compareCollated((std::string)"Hello world", (std::string)""), 1);
    AssertEq(compareCollated((std::string)"Hello world", (std::string)"Aaron"), 1);
    AssertEq(compareCollated((std::string)"Hello world", (std::string)"Hello world!"), -1);
    AssertEq(compareCollated((std::string)"hello World", (std::string)"hellO wOrLd"), -1); // uppercase letters win ties
    AssertEq(compareCollated((std::string)"Hello world", (std::string)"jello world"), -1); // but letter order comes first
    AssertEq(compareCollated((std::string)"hello world", (std::string)"Jello world"), -1);

    // Non-ASCII characters aren't going to sort according to the Unicode Collation Algorithm,
    // but they should still sort after all ASCII characters.
    AssertEq(compareCollated((std::string)"Hello world", (std::string)"Hello wörld!"), -1);
}

- (void) testIndexKey {
    std::string key = "OR";
    Collatable collKey;
    collKey << key;

    std::string docID = "foo";
    Collatable collatableDocID;
    collatableDocID << docID;

    Collatable indexKey;
    indexKey.beginArray();
    indexKey << collKey << collatableDocID << (int64_t)1234;
    indexKey.endArray();

    alloc_slice encoded((forestdb::slice)indexKey);

    CollatableReader reader(encoded);
    reader.beginArray();
    forestdb::slice readKey = reader.read();
    Assert(readKey.equal((forestdb::slice)collKey));
    alloc_slice readDocID = reader.readString();
    Assert(readDocID.equal((forestdb::slice)docID));
    int64_t readSequence = reader.readInt();
    AssertEq(readSequence, 1234);
}

@end
