//
//  Tokenizer_Tests.m
//  CBForest
//
//  Created by Jens Alfke on 5/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import <CBForest/CBForest.h>
#import "CBForestPrivate.h"


@interface Tokenizer_Tests : XCTestCase
@end


@implementation Tokenizer_Tests
{
    CBTextTokenizer* tokenizer;
}

- (void)setUp {
    [super setUp];
}

- (void)tearDown {
    [super tearDown];
}

- (NSArray*) tokenize: (NSString*)string {
    NSMutableArray* tokens = [NSMutableArray array];
    [tokenizer tokenize: string onToken: ^(NSString* token, NSRange r) {
        XCTAssertNotNil(token);
        XCTAssert(r.length > 0 && r.length < 20);
        XCTAssert(r.location < string.length);
        [tokens addObject: token];
    }];

    XCTAssertEqualObjects([NSSet setWithArray: tokens], [tokenizer tokenize: string]);

    return tokens;
}

- (void)testDefaultTokenizer {
    tokenizer = [[CBTextTokenizer alloc] initWithLanguage: nil removeDiacritics: NO];

    XCTAssertEqualObjects(([self tokenize: @"Have a nice day, dude!"]),
                          (@[@"have", @"a", @"nice", @"day", @"dude"]));
    XCTAssertEqualObjects(([self tokenize: @"Having,larger books. ¡Ça vä!"]),
                          (@[@"having", @"larger", @"books", @"ça", @"vä"]));
    XCTAssertEqualObjects(([self tokenize: @"“Typographic ‘quotes’ aren’t optional”"]),
                          (@[@"typographic", @"quotes", @"aren", @"t", @"optional"]));
}

- (void)testEnglishTokenizer {
    tokenizer = [[CBTextTokenizer alloc] initWithLanguage: @"en" removeDiacritics: YES];

    XCTAssertEqualObjects(([self tokenize: @"Have a nice day, dude!"]),
                          (@[@"nice", @"day", @"dude"]));
    XCTAssertEqualObjects(([self tokenize: @"Having,larger books. ¡Ça vä!"]),
                          (@[@"larger", @"book", @"ca", @"va"]));
    XCTAssertEqualObjects(([self tokenize: @"\"Typographic 'quotes' can't be optional\""]),
                          (@[@"typograph", @"quot", @"option"]));
    XCTAssertEqualObjects(([self tokenize: @"“Typographic ‘quotes’ can’t be optional”"]),
                          (@[@"typograph", @"quot", @"option"]));
}

@end
