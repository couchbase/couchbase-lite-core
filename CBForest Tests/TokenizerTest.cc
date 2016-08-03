//
//  Tokenizer_Test.m
//  CBForest
//
//  Created by Jens Alfke on 10/20/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Tokenizer.hh"
#include <string>
#include <vector>
#include <iostream>

using namespace std;

static std::ostream& operator<< (std::ostream& o, vector<string> v) {
    o << "{";
    for (auto str : v)
        o << str << ", ";
    o << "}";
    return o;
}


#include "CBForestTest.hh"

using namespace fleece;


class TokenizerTest : public DatabaseTestFixture {

    unique_ptr<Tokenizer> tokenizer;


    vector<string> tokenize(string text, bool unique =false) {
        vector<string> tokens;
        for (TokenIterator i(*tokenizer, slice(text), unique); i; ++i) {
            Assert(i.wordLength() > 0 && i.wordLength() < 20);
            Assert(i.wordOffset() < text.size());
            tokens.push_back(i.token());
        }
        return tokens;
    }


    void testDefaultTokenizer() {
        tokenizer.reset(new Tokenizer("", false));
        AssertEqual(tokenize("Have a nice day, dude!"),
                    (vector<string>{"have", "a", "nice", "day", "dude"}));
        AssertEqual(tokenize("Having,larger books. ¡Ça vä!"),
                    (vector<string>{"having", "larger", "books", "ça", "vä"}));
        AssertEqual(tokenize("“Typographic ‘quotes’ aren’t optional”"),
                    (vector<string>{"typographic", "quotes", "aren't", "optional"}));
        AssertEqual(tokenize("seven eight seven nine", true),
                    (vector<string>{"seven", "eight", "nine"}));
    }

    void testEnglishTokenizer() {
        tokenizer.reset(new Tokenizer("english", true));

        AssertEqual(tokenize("Have a nice day, dude!"),
                    (vector<string>{"nice", "day", "dude"}));
        AssertEqual(tokenize("Having,larger books. ¡Ça vä!"),
                    (vector<string>{"larger", "book", "ca", "va"}));
        AssertEqual(tokenize("\"Typographic 'quotes' can't be optional\""),
                    (vector<string>{"typograph", "quot", "option"}));
        AssertEqual(tokenize("“Typographic ‘quotes’ can’t be optional”"),
                    (vector<string>{"typograph", "quot", "option"}));
        AssertEqual(tokenize("seven can't nine"),
                    (vector<string>{"seven", "nine"}));
        AssertEqual(tokenize("seven can’t nine"),  // curly quote!
                    (vector<string>{"seven", "nine"}));
    }

    CPPUNIT_TEST_SUITE( TokenizerTest );
    CPPUNIT_TEST( testDefaultTokenizer );
    CPPUNIT_TEST( testEnglishTokenizer );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(TokenizerTest);
