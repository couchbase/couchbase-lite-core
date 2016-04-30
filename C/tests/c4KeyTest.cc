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


class C4KeyTest : public CppUnit::TestFixture {
public:

    C4Key *key;

    virtual void setUp() {
        key = c4key_new();
    }

    virtual void tearDown() {
        c4key_free(key);
    }

    void populateKey() {
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

    void testCreateKey() {
        populateKey();
        AssertEqual(toJSON(key), std::string("[null,false,true,0,12345,-2468,\"foo\",[]]"));
    }

    void testReadKey() {
        populateKey();
        C4KeyReader r = c4key_read(key);
        AssertEqual(c4key_peek(&r), (uint8_t)kC4Array);
        c4key_skipToken(&r);
        AssertEqual(c4key_peek(&r), (uint8_t)kC4Null);
        c4key_skipToken(&r);
        AssertEqual(c4key_peek(&r), (uint8_t)kC4Bool);
        AssertEqual(c4key_readBool(&r), false);
        AssertEqual(c4key_readBool(&r), true);
        AssertEqual(c4key_readNumber(&r), 0.0);
        AssertEqual(c4key_readNumber(&r), 12345.0);
        AssertEqual(c4key_readNumber(&r), -2468.0);
        AssertEqual(c4key_readString(&r), c4str("foo"));
        AssertEqual(c4key_peek(&r), (uint8_t)kC4Array);
        c4key_skipToken(&r);
        AssertEqual(c4key_peek(&r), (uint8_t)kC4EndSequence);
        c4key_skipToken(&r);
        AssertEqual(c4key_peek(&r), (uint8_t)kC4EndSequence);
        c4key_skipToken(&r);
    }


    CPPUNIT_TEST_SUITE( C4KeyTest );
    CPPUNIT_TEST( testCreateKey );
    CPPUNIT_TEST( testReadKey );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(C4KeyTest);
