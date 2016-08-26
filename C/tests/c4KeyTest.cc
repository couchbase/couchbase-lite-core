//
//  c4KeyTest.cc
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4View.h"
#include "c4Document.h"


class C4KeyTest {
public:

    C4Key *key;

    C4KeyTest() {
        key = c4key_new();
        c4key_beginArray(key);
        c4key_addNull(key);
        c4key_addBool(key, false);
        c4key_addBool(key, true);
        c4key_addNumber(key, 0);
        c4key_addNumber(key, 12345);
        c4key_addNumber(key, -2468);
        c4key_addString(key, c4str("foo"));
        c4key_beginArray(key);
        c4key_endArray(key);
        c4key_endArray(key);
    }

    ~C4KeyTest() {
        c4key_free(key);
    }
};


TEST_CASE_METHOD(C4KeyTest, "CreateKey", "[Key][C]") {
    REQUIRE(toJSON(key) == std::string("[null,false,true,0,12345,-2468,\"foo\",[]]"));
}

TEST_CASE_METHOD(C4KeyTest, "ReadKey", "[Key][C]") {
    C4KeyReader r = c4key_read(key);
    REQUIRE(c4key_peek(&r) == (C4KeyToken)kC4Array);
    c4key_skipToken(&r);
    REQUIRE(c4key_peek(&r) == (C4KeyToken)kC4Null);
    c4key_skipToken(&r);
    REQUIRE(c4key_peek(&r) == (C4KeyToken)kC4Bool);
    REQUIRE(c4key_readBool(&r) == false);
    REQUIRE(c4key_readBool(&r) == true);
    REQUIRE(c4key_readNumber(&r) == 0.0);
    REQUIRE(c4key_readNumber(&r) == 12345.0);
    REQUIRE(c4key_readNumber(&r) == -2468.0);
    REQUIRE(c4key_readString(&r) == c4str("foo"));
    REQUIRE(c4key_peek(&r) == (C4KeyToken)kC4Array);
    c4key_skipToken(&r);
    REQUIRE(c4key_peek(&r) == (C4KeyToken)kC4EndSequence);
    c4key_skipToken(&r);
    REQUIRE(c4key_peek(&r) == (C4KeyToken)kC4EndSequence);
    c4key_skipToken(&r);
}
