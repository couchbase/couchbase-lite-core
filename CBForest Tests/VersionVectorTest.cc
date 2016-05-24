//
//  VersionVectorTest.cc
//  CBForest
//
//  Created by Jens Alfke on 5/24/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "RevID.hh"
#include "VersionVector.hh"
#include <iostream>
using namespace cbforest;

// Some operators to make slice work with AssertEqual:
// (This has to be declared before including cppunit, because C++ sucks)
static std::ostream& operator<< (std::ostream& o, const versionVector &v) {
    o << (std::string)v.asString();
    return o;
}

static std::ostream& operator<< (std::ostream& o, const version &v) {
    o << v.gen << "@" << std::string((char*)v.peer.buf, v.peer.size);
    return o;
}

#include "CBForestTest.hh"


static revidBuffer stringToRev(const char* str) {
    revidBuffer buf(slice(str), true);
    return buf;
}

static versionVector& operator << (versionVector &v, const char *str) {
    v.append( version(slice(str)) );
    return v;
}


class VersionVectorTest : public CppUnit::TestFixture {

    void testRevIDs() {
        revidBuffer rev(slice("1-f0f0"));
        AssertEqual((std::string)rev, std::string("1-f0f0"));
        static const uint8_t expectedBytes[] = {0x01, 0xf0, 0xf0};
        Assert(rev == slice(expectedBytes, sizeof(expectedBytes)));

        rev = stringToRev("1234-1234567890abcdef1234567890abcdef");
        AssertEqual((std::string)rev, std::string("1234-1234567890abcdef1234567890abcdef"));
        static const uint8_t expectedBytes2[18] = {0xd2, 0x09, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD,
            0xEF, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF};
        Assert(rev == slice(expectedBytes2, sizeof(expectedBytes2)));

        // New-style ('clock') revID:
        rev.parseNew(slice("17@snej"));
        Assert(rev.isClock());
        AssertEqual(rev.generation(), 17u);
        AssertEqual(rev.digest(), slice("snej"));
        static const uint8_t expectedBytes3[] = {0x00, 0x11, 's', 'n', 'e', 'j'};
        AssertEqual((slice)rev, slice(expectedBytes3, sizeof(expectedBytes3)));
    }

    void testBadRevIDs() {
        // Check a bunch of invalid revIDs to make sure they all correctly fail to parse:
        static const char* kBadStrings[] = {
            "",
            "@",
            "1@",
            "@snej",
            "snej@x",
            "0@snej",
            "12345678901234567890123@snej",
            "1234@abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
            "-",
            "1-",
            "-ff",
            "1-snej",
            "1-abc",
            "0-cafe",
            "1-123",
            "12345678901234567890123-cafe",
            "123-f@bb",
        };
        for (int i = 0; i < sizeof(kBadStrings)/sizeof(char*); i++) {
            revidBuffer rev;
            Assert(!rev.tryParse(slice(kBadStrings[i]), true));
        }
        
        // Make sure we don't parse new-style IDs with the old parser:
        revidBuffer rev;
        Assert(!rev.tryParse(slice("17@snej"), false));
    }
    
    
    void testCreate() {
        versionVector v;
        v << "1@jens" << "2@bob";
        AssertEqual(v[slice("jens")], 1ull);
        AssertEqual(v[slice("bob")],  2ull);
        AssertEqual(v[slice("may")],  0ull);
        AssertEqual(v.current(), version(1, slice("jens")));
        AssertEqual(v.count(), 2ul);

        alloc_slice str = v.asString();
        AssertEqual((slice)v.asString(), slice("1@jens,2@bob"));

        versionVector vv(str);
        AssertEqual(vv, v);
        AssertEqual(vv.asString(), v.asString());
    }

    void testCreateSingle() {
        versionVector v(slice("1@jens"));
        AssertEqual(v[slice("jens")], 1ull);
        AssertEqual(v[slice("bob")],  0ull);
        AssertEqual(v.current(), version(1, slice("jens")));
        AssertEqual(v.count(), 1ul);
        AssertEqual((slice)v.asString(), slice("1@jens"));
    }

    void testCompare() {
        versionVector v;
        v << "1@jens" << "2@bob";
        AssertEqual(v, v);
        Assert(!(v > v));
        Assert(!(v < v));
        AssertEqual(v.compareTo(v), versionVector::kSame);

        versionVector oldv;
        oldv << "2@bob";

        Assert(!(v == oldv));
        Assert(v > oldv);
        Assert(oldv < v);
        AssertEqual(v.compareTo(oldv), versionVector::kNewer);
        AssertEqual(oldv.compareTo(v), versionVector::kOlder);

        versionVector otherV(slice("3@bob"));
        AssertEqual(v.compareTo(otherV), versionVector::kConflicting);
    }

    void testIncrement() {
        versionVector v(slice("123@jens,3141592654@bob"));
        v.incrementGenOfPeer(slice("bob"));

        AssertEqual(v[slice("jens")], 123ull);
        AssertEqual(v[slice("bob")],  3141592655ull);
        AssertEqual(v.current(), version(3141592655, slice("bob")));
        AssertEqual(v.count(), 2ul);

        alloc_slice str = v.asString();
        AssertEqual((slice)str, slice("3141592655@bob,123@jens"));

        v.incrementGenOfPeer(slice("may"));

        AssertEqual(v[slice("jens")], 123ull);
        AssertEqual(v[slice("bob")],  3141592655ull);
        AssertEqual(v[slice("may")],  1ull);
        AssertEqual(v.current(), version(1, slice("may")));
        AssertEqual(v.count(), 3ul);

        str = v.asString();
        AssertEqual((slice)str, slice("1@may,3141592655@bob,123@jens"));
    }

    void testIncrementEmpty() {
        versionVector v;
        v.incrementGenOfPeer(slice("may"));
        AssertEqual(v[slice("may")],  1ull);
        AssertEqual(v.current(), version(1, slice("may")));
        AssertEqual(v.count(), 1ul);
        AssertEqual((slice)v.asString(), slice("1@may"));
    }

    void testMerge(const char *str1, const char *str2, const char *expectedStr) {
        versionVector v1((slice(str1))), v2((slice(str2)));
        versionVector result = v1.mergedWith(v2);
        alloc_slice resultStr = result.asString();
        AssertEqual((slice)resultStr, (slice)expectedStr);
    }

    void testMerge() {
        testMerge("19@jens",             "1@bob",               "19@jens,1@bob");
        testMerge("19@jens",             "18@jens",             "19@jens");
        testMerge("18@jens",             "19@jens",             "19@jens");
        testMerge("18@jens,1@bob",       "19@jens",             "19@jens,1@bob");
        testMerge("19@jens,1@bob",       "2@bob,18@jens",       "19@jens,2@bob");
        testMerge("2@bob,18@jens",       "19@jens,1@bob",       "2@bob,19@jens");
        testMerge("19@jens,3@eve,1@bob", "2@bob,18@jens,3@eve", "19@jens,2@bob,3@eve");
        testMerge("2@bob,18@jens,3@eve", "19@jens,3@eve,1@bob", "2@bob,19@jens,3@eve");
    }


    CPPUNIT_TEST_SUITE( VersionVectorTest );
    CPPUNIT_TEST( testRevIDs );
    CPPUNIT_TEST( testBadRevIDs );
    CPPUNIT_TEST( testCreate );
    CPPUNIT_TEST( testCreateSingle );
    CPPUNIT_TEST( testCompare );
    CPPUNIT_TEST( testIncrement );
    CPPUNIT_TEST( testIncrementEmpty );
    CPPUNIT_TEST( testMerge );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(VersionVectorTest);
