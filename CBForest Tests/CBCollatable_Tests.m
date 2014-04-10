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

- (void)setUp
{
    [super setUp];
    // Put setup code here. This method is called before the invocation of each test method in the class.
}

- (void)tearDown
{
    // Put teardown code here. This method is called after the invocation of each test method in the class.
    [super tearDown];
}

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
    XCTAssertEqual(collate(@"Hello world", @"Aaron"), 1);
    XCTAssertEqual(collate(@"Hello world", @""), 1);
    XCTAssertEqual(collate(@"Hello world", @"hellO wOrLd"), 0); //FIX: This should be -1
    XCTAssertEqual(collate(@"Hello world", @"hellO wOrLd!"), -1);
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

@end
