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

using namespace cbforest;

@interface Collatable_Test : XCTestCase
@end

@implementation Collatable_Test

template <typename T>
static int sgn(T n) {return n<0 ? -1 : (n>0 ? 1 : 0);}

template <typename T1, typename T2>
static int compareCollated(T1 obj1, T2 obj2) {
    CollatableBuilder c1, c2;
    c1 << obj1;
    c2 << obj2;
    return sgn(cbforest::slice(c1).compare(cbforest::slice(c2)));
}

static NSData* collatableData(id obj) {
    CollatableBuilder c(obj);
    return ((slice)c).copiedNSData();
}

static uint64_t randn(uint64_t limit) {
    uint64_t n;
    (void)SecRandomCopyBytes(kSecRandomDefault, 8, (uint8_t*)&n);
    return n % limit;
}

static double randf() {
    union {double d; struct {uint32_t u1, u2;};} n;
    do {
        n.u1 = (uint32_t)random();
        n.u2 = (uint32_t)random();
    } while (isnan(n.d) || isinf(n.d));
    return n.d;
}

static id roundTrip(id input) {
    CollatableBuilder c;
    c << input;
    alloc_slice encoded((cbforest::slice)c);
    CollatableReader reader(encoded);
    return reader.readNSObject();
}

- (void) checkRoundTrip: (id)input {
    AssertEqual(roundTrip(input), input);
    // Note: isEqual: has some limitations when comparing NSNumbers. If one number is a double it
    // seems to convert the other number to double and then compare; this can produce false
    // positives when the other number is a very large 64-bit integer that can't be precisely
    // represented as a double (basically anything above 2^56 or so.)
}

- (void) compareNumber: (NSNumber*)n1 with: (NSNumber*)n2 {
    bool correct = compareCollated(n1, n2) == [n1 compare: n2];
    if (!correct)
        NSLog(@"%@: %@ vs %@ -- %@ vs %@",
              (correct ? @"yes" : @"NO "),
              n1, n2,
              collatableData(n1), collatableData(n2));
    XCTAssert(correct);
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
        [self compareNumber: @(n1) with: @(n2)];
    }
}

- (void) testFloats {
    double numbers[] = {0, 1, 2, 10, 32, 63, 64, 256, M_PI, 100, 6.02e23, 6.02e-23, 0.01,
        DBL_MAX, DBL_MIN,
        M_PI + 0.1, M_PI - 0.1,
        -1, -64, -M_PI, -6.02e23};
    const int nFloats = sizeof(numbers)/sizeof(numbers[0]);
    for (int i=0; i<nFloats; i++) {
        id n1 = @(numbers[i]);
        NSLog(@"%16g --> %@", numbers[i], collatableData(n1));
        [self checkRoundTrip: n1];
        for (int j=0; j<nFloats; j++) {
            [self compareNumber: n1 with: @(numbers[j])];
        }
    }
}

- (void) testRandomFloats {
    srandomdev();
    self.continueAfterFailure = NO;
    for (int i=0; i< 100000; i++) {
        @autoreleasepool {
            NSNumber *n1 = @(randf()), *n2 = @(randf());
            //NSLog(@"Compare: %@ <--> %@", n1, n2);
            [self checkRoundTrip: n1];
            [self checkRoundTrip: n2];
            [self compareNumber: n1 with: n2];
        }
    }
}

- (void) testRoundTripInts {
    uint64_t n = 1;
    for (int bits = 0; bits < 64; ++bits, n<<=1) {
        CollatableBuilder c;
        c << n - 1;
        alloc_slice encoded((cbforest::slice)c);
        CollatableReader reader(encoded);
        uint64_t result = [reader.readNSObject() unsignedLongLongValue];
        NSLog(@"2^%2d - 1: %llx --> %llx", bits, n-1, result);
        // At 2^54-1 floating-point roundoff starts to occur. This is known, so skip the assert
        if (bits < 54)
            AssertEq(result, n-1);
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

    // Make sure nulls and control characters don't break anything:
    [self checkRoundTrip: @"foo\0"];
    [self checkRoundTrip: @"foo\0\1\2bar"];
    [self checkRoundTrip: @"\033\034\035"];
    // DEL is weird. There isn't room in the Collatable encoding to give it a unique value, so it
    // gets the same value as space, meaning it decodes to space. Make sure it at least doesn't
    // cause an error:
    Assert([roundTrip(@"hey\177there") hasPrefix: @"hey"]);
    Assert([roundTrip(@"hey\177there") hasSuffix: @"there"]);
}

- (void) testIndexKey {
    std::string key = "OR";
    CollatableBuilder collKey;
    collKey << key;

    std::string docID = "foo";
    CollatableBuilder collatableDocID;
    collatableDocID << docID;

    CollatableBuilder indexKey;
    indexKey.beginArray();
    indexKey << collKey << collatableDocID << (int64_t)1234;
    indexKey.endArray();

    alloc_slice encoded((cbforest::slice)indexKey);

    CollatableReader reader(encoded);
    reader.beginArray();
    cbforest::slice readKey = reader.read();
    Assert(readKey == (cbforest::slice)collKey);
    alloc_slice readDocID = reader.readString();
    Assert(readDocID == (cbforest::slice)docID);
    int64_t readSequence = reader.readInt();
    AssertEq(readSequence, 1234);
}

static NSString* toJSON(Collatable c) {
    std::string json = c.toJSON();
    return [[NSString alloc] initWithBytes: json.data() length: json.size()
                                  encoding: NSUTF8StringEncoding];
}

- (void) testDump {
    CollatableBuilder("howdy");
    CollatableBuilder c;
    c.addBool(false);
    AssertEqual(toJSON(c), @"false");
    c = CollatableBuilder();
    c.addBool(true);
    AssertEqual(toJSON(c), @"true");
    AssertEqual(toJSON(CollatableBuilder(66)), @"66");

    AssertEqual(toJSON(CollatableBuilder("howdy")), @"\"howdy\"");
    AssertEqual(toJSON(CollatableBuilder("\"ironic\"")), @"\"\\\"ironic\\\"\"");
    AssertEqual(toJSON(CollatableBuilder("an \"ironic\" twist")), @"\"an \\\"ironic\\\" twist\"");
    AssertEqual(toJSON(CollatableBuilder("\\foo\\")), @"\"\\\\foo\\\\\"");
    AssertEqual(toJSON(CollatableBuilder("\tline1\nline2\t")), @"\"\\tline1\\nline2\\t\"");
    AssertEqual(toJSON(CollatableBuilder("line1\01\02line2")), @"\"line1\\u0001\\u0002line2\"");

    c = CollatableBuilder();
    c.beginArray();
    c << 1234;
    c.endArray();
    AssertEqual(toJSON(c), @"[1234]");

    c = CollatableBuilder();
    c.beginArray();
    c << 1234;
    c << 5678;
    c.endArray();
    AssertEqual(toJSON(c), @"[1234,5678]");

    c = CollatableBuilder();
    c.beginMap();
    c << "name";
    c << "Frank";
    c << "age";
    c << 11;
    c.endMap();
    AssertEqual(toJSON(c), @"{\"name\":\"Frank\",\"age\":11}");
}

@end
