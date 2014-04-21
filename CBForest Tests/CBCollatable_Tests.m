//
//  CBCollatable_Tests.m
//  CBForest
//
//  Created by Jens Alfke on 4/9/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "CBCollatable.h"
#import "CBForestPrivate.h"


@interface CBCollatable_Tests : XCTestCase
@end


@implementation CBCollatable_Tests

static int collate(id obj1, id obj2) {
    NSData* data1 = CBCreateCollatable(obj1);
    NSData* data2 = CBCreateCollatable(obj2);
    int cmp = CompareBufs(DataToBuf(data1), DataToBuf(data2));
    return cmp<0 ? -1 : (cmp>0 ? 1 : 0);
}

static uint64_t randn(uint64_t limit) {
    uint64_t n;
    SecRandomCopyBytes(kSecRandomDefault, 8, (void*)&n);
    return n % limit;
}

- (void) compareNumber: (NSNumber*)n1 with: (NSNumber*)n2 {
    XCTAssertEqual(collate(n1, n2), [n1 compare: n2],
                   @"Failed collation of %@ with %@: %@ vs %@",
                   n1, n2, CBCreateCollatable(n1), CBCreateCollatable(n2));
}

- (void)testNumbers {
    XCTAssertEqual(collate(@YES, @NO), 1);
    XCTAssertEqual(collate(@NO, [NSNull null]), 1);
    XCTAssertEqual(collate(@YES, @17), -1);
    XCTAssertEqual(collate(@1, @1), 0);
    XCTAssertEqual(collate(@123, @1), 1);
    XCTAssertEqual(collate(@0x100, @0xFF), 1);
    XCTAssertEqual(collate(@0x1234, @0x12), 1);
    XCTAssertEqual(collate(@0x1234, @0x13), 1);
    XCTAssertEqual(collate(@(INT64_MAX), @(INT32_MAX)), 1);

    XCTAssertEqual(collate(@(-1), @(0)), -1);
    XCTAssertEqual(collate(@(-1), @(1)), -1);
    XCTAssertEqual(collate(@(-123), @(-7)), -1);
    [self compareNumber: @(-257) with: @(-1000)];
    [self compareNumber: @(-256) with: @(-1)];
    [self compareNumber: @(-255) with: @(-1)];
#if 1
    // Compare a bunch of random unsigned 64-bit numbers:
    self.continueAfterFailure = NO;
    for (int i=0; i< 10000; i++) {
        int64_t n1, n2;
        n1 = (int64_t)randn(UINT64_MAX) >> randn(63);
        n2 = (int64_t)randn(UINT64_MAX) >> randn(63);
        [self compareNumber: @(n1) with: @(n2)];
    }
#endif
}

- (void) testStrings {
    XCTAssertEqual(collate(@"", @7), 1);
    XCTAssertEqual(collate(@"", @""), 0);
    XCTAssertEqual(collate(@"", @YES), 1);
    XCTAssertEqual(collate(@"", [NSNull null]), 1);
    XCTAssertEqual(collate(@"", @" "), -1);
    XCTAssertEqual(collate(@"~", @"a"), -1);
    XCTAssertEqual(collate(@"A", @"a"), 1);
    XCTAssertEqual(collate(@"\n", @" "), -1);
    XCTAssertEqual(collate(@"Hello world", @""), 1);
    XCTAssertEqual(collate(@"Hello world", @"Aaron"), 1);
    XCTAssertEqual(collate(@"Hello world", @"Hello world!"), -1);
    XCTAssertEqual(collate(@"hello World", @"hellO wOrLd"), -1); // uppercase letters win ties
    XCTAssertEqual(collate(@"Hello world", @"jello world"), -1); // but letter order comes first
    XCTAssertEqual(collate(@"hello world", @"Jello world"), -1);

    // Non-ASCII characters aren't going to sort according to the Unicode Collation Algorithm,
    // but they should still sort after all ASCII characters.
    XCTAssertEqual(collate(@"Hello world", @"Hello wörld!"), -1);
}

- (void) testArrays {
    XCTAssertEqual(collate(@[], @""), 1);
    XCTAssertEqual(collate(@[], @[@1]), -1);
    XCTAssertEqual(collate(@[@"hi", @"there"], @[@"hi"]), 1);
}

- (void) testDicts {
    XCTAssertEqual(collate(@{}, @[]), 1);
    XCTAssertEqual(collate(@{}, @{@"happy?":@NO}), -1);
    XCTAssertEqual(collate(@{@"a": @7}, @{@"a": @7, @"b": @8}), -1);
    XCTAssertEqual(collate(@{@"a": @7}, @{@"a": @8}), -1);
}

- (void) roundTrip: (id)value type: (CBCollatableType) type {
    NSData* collatable = CBCreateCollatable(value);
    XCTAssert(collatable != nil);
    // First use ReadNext:
    sized_buf buf = DataToBuf(collatable);
    id output;
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), type);
    if (![value isKindOfClass: [NSArray class]] && ![value isKindOfClass: [NSDictionary class]]) {
        XCTAssertEqualObjects(output, value);
        XCTAssertEqual(buf.size, 0);
    }

    // Now try Read:
    XCTAssertEqualObjects(CBCollatableRead(DataToBuf(collatable)), value);
}

- (void) testReadScalars {
    [self roundTrip: [NSNull null] type: kNullType];
    [self roundTrip: @NO type: kFalseType];
    [self roundTrip: @YES type: kTrueType];
    [self roundTrip: @0 type: kNumberType];
    [self roundTrip: @1 type: kNumberType];
    [self roundTrip: @12345 type: kNumberType];
    [self roundTrip: @1234512345 type: kNumberType];
    [self roundTrip: @-1 type: kNumberType];
    [self roundTrip: @-12345 type: kNumberType];
    [self roundTrip: @"" type: kStringType];
    [self roundTrip: @"Four score & 7 yrs. ago" type: kStringType];
    [self roundTrip: @"I &heart; ÜTF-∞" type: kStringType];

    [self roundTrip: @[] type: kArrayType];
    [self roundTrip: @[@"foo", @3141, @NO, @"bär"] type: kArrayType];
    [self roundTrip: @[@YES, @[]] type: kArrayType];

    [self roundTrip: @{} type: kDictionaryType];
    [self roundTrip: @{@"key": @"value"} type: kDictionaryType];
    [self roundTrip: @{@"key": @"value", @"": [NSNull null]} type: kDictionaryType];
}

- (void) testReadArray {
    NSData* collatable = CBCreateCollatable(@[@"foo", @3141, @NO, @"bär"]);
    XCTAssert(collatable != nil);
    sized_buf buf = DataToBuf(collatable);
    id output;
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kArrayType);
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kStringType);
    XCTAssertEqualObjects(output, @"foo");
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kNumberType);
    XCTAssertEqualObjects(output, @3141);
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kFalseType);
    XCTAssertEqualObjects(output, @NO);
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kStringType);
    XCTAssertEqualObjects(output, @"bär");
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kEndSequenceType);
    XCTAssertEqual(buf.size, 0);
}

- (void) testReadDictionary {
    NSData* collatable = CBCreateCollatable(@{@"key": @-2});
    XCTAssert(collatable != nil);
    sized_buf buf = DataToBuf(collatable);
    id output;
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kDictionaryType);
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kStringType);
    XCTAssertEqualObjects(output, @"key");
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kNumberType);
    XCTAssertEqualObjects(output, @-2);
    XCTAssertEqual(CBCollatableReadNext(&buf, &output), kEndSequenceType);
    XCTAssertEqual(buf.size, 0);
}

@end
