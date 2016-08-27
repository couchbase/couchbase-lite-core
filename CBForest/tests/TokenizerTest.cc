//
//  Tokenizer_Test.m
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 10/20/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//

#include "Tokenizer.hh"
#include <string>
#include <vector>
#include <iostream>
#include "CBLCoreTest.hh"


class TokenizerTest : public DataFileTestFixture {
public:
    unique_ptr<Tokenizer> tokenizer;

    vector<string> tokenize(string text, bool unique =false) {
        vector<string> tokens;
        for (TokenIterator i(*tokenizer, slice(text), unique); i; ++i) {
            REQUIRE(i.wordLength() > 0);
            REQUIRE(i.wordLength() < 20);
            REQUIRE(i.wordOffset() < text.size());
            tokens.push_back(i.token());
        }
        return tokens;
    }

};


TEST_CASE_METHOD(TokenizerTest, "Default Tokenizer", "[Tokenizer]") {
    tokenizer.reset(new Tokenizer("", false));
    REQUIRE(tokenize("Have a nice day, dude!") == (vector<string>{"have", "a", "nice", "day", "dude"}));
    REQUIRE(tokenize("Having,larger books. ¡Ça vä!") == (vector<string>{"having", "larger", "books", "ça", "vä"}));
    REQUIRE(tokenize("“Typographic ‘quotes’ aren’t optional”") == (vector<string>{"typographic", "quotes", "aren't", "optional"}));
    REQUIRE(tokenize("seven eight seven nine", true) == (vector<string>{"seven", "eight", "nine"}));
}


TEST_CASE_METHOD(TokenizerTest, "English Tokenizer", "[Tokenizer]") {
    tokenizer.reset(new Tokenizer("english", true));

    REQUIRE(tokenize("Have a nice day, dude!") == (vector<string>{"nice", "day", "dude"}));
    REQUIRE(tokenize("Having,larger books. ¡Ça vä!") == (vector<string>{"larger", "book", "ca", "va"}));
    REQUIRE(tokenize("\"Typographic 'quotes' can't be optional\"") == (vector<string>{"typograph", "quot", "option"}));
    REQUIRE(tokenize("“Typographic ‘quotes’ can’t be optional”") == (vector<string>{"typograph", "quot", "option"}));
    REQUIRE(tokenize("seven can't nine") == (vector<string>{"seven", "nine"}));
    REQUIRE(tokenize("seven can’t nine") ==  // curly quote!
                (vector<string>{"seven", "nine"}));
}
