//
//  Tokenizer_Test.m
//  CBForest
//
//  Created by Jens Alfke on 10/20/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "Tokenizer.hh"

using namespace cbforest;


@interface Tokenizer_Test : XCTestCase
@end


@implementation Tokenizer_Test
{
    Tokenizer* tokenizer;
}

- (void) setUp {
    [super setUp];
}

- (void) tearDown {
    delete tokenizer;
    [super tearDown];
}

- (NSArray*) tokenize: (NSString*)string unique: (BOOL)unique {
    NSMutableArray* tokens = [NSMutableArray array];
    for (TokenIterator i(*tokenizer, nsstring_slice(string), unique); i; ++i) {
        std::string tok = i.token();
        NSString* token = [[NSString alloc] initWithBytes: tok.c_str() length: tok.size() encoding: NSUTF8StringEncoding];
        XCTAssert(i.wordLength() > 0 && i.wordLength() < 20);
        XCTAssert(i.wordOffset() < string.length);
        [tokens addObject: token];
    }
    return tokens;
}

- (NSArray*) tokenize: (NSString*)string {
    return [self tokenize: string unique: NO];
}

- (void)testDefaultTokenizer {
    tokenizer = new Tokenizer("", false);
    XCTAssertEqualObjects(([self tokenize: @"Have a nice day, dude!"]),
                          (@[@"have", @"a", @"nice", @"day", @"dude"]));
    XCTAssertEqualObjects(([self tokenize: @"Having,larger books. ¡Ça vä!"]),
                          (@[@"having", @"larger", @"books", @"ça", @"vä"]));
    XCTAssertEqualObjects(([self tokenize: @"“Typographic ‘quotes’ aren’t optional”"]),
                          (@[@"typographic", @"quotes", @"aren't", @"optional"]));
    XCTAssertEqualObjects(([self tokenize: @"seven eight seven nine" unique: YES]),
                          (@[@"seven", @"eight", @"nine"]));
}

- (void)testEnglishTokenizer {
    tokenizer = new Tokenizer("english", true);

    XCTAssertEqualObjects(([self tokenize: @"Have a nice day, dude!"]),
                          (@[@"nice", @"day", @"dude"]));
    XCTAssertEqualObjects(([self tokenize: @"Having,larger books. ¡Ça vä!"]),
                          (@[@"larger", @"book", @"ca", @"va"]));
    XCTAssertEqualObjects(([self tokenize: @"\"Typographic 'quotes' can't be optional\""]),
                          (@[@"typograph", @"quot", @"option"]));
    XCTAssertEqualObjects(([self tokenize: @"“Typographic ‘quotes’ can’t be optional”"]),
                          (@[@"typograph", @"quot", @"option"]));
    XCTAssertEqualObjects(([self tokenize: @"seven can't nine"]),
                          (@[@"seven", @"nine"]));
    XCTAssertEqualObjects(([self tokenize: @"seven can’t nine"]),  // curly quote!
                          (@[@"seven", @"nine"]));
}


@end
