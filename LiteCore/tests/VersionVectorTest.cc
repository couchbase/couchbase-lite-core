//
//  VersionVectorTest.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/24/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#include "RevID.hh"
#include "VersionVector.hh"
#include "Fleece.hh"
#include <iostream>
using namespace std;
using namespace litecore;

// This has to come last for obscure C++ reasons
#include "LiteCoreTest.hh"


static revidBuffer stringToRev(const char* str) {
    revidBuffer buf(slice(str), true);
    return buf;
}


TEST_CASE("RevIDs", "[VersionVector]") {
    revidBuffer rev(slice("1-f0f0"));
    REQUIRE((std::string)rev == std::string("1-f0f0"));
    static const uint8_t expectedBytes[] = {0x01, 0xf0, 0xf0};
    REQUIRE(rev == slice(expectedBytes, sizeof(expectedBytes)));

    rev = stringToRev("1234-1234567890abcdef1234567890abcdef");
    REQUIRE((std::string)rev == std::string("1234-1234567890abcdef1234567890abcdef"));
    static const uint8_t expectedBytes2[18] = {0xd2, 0x09, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD,
        0xEF, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF};
    REQUIRE(rev == slice(expectedBytes2, sizeof(expectedBytes2)));

    // New-style ('clock') revID:
    rev.parseNew(slice("17@snej"));
    REQUIRE(rev.isClock());
    REQUIRE(rev.generation() == 17u);
    REQUIRE(rev.digest() == slice("snej"));
    static const uint8_t expectedBytes3[] = {0x00, 0x11, 's', 'n', 'e', 'j'};
    REQUIRE((slice)rev == slice(expectedBytes3, sizeof(expectedBytes3)));
}

TEST_CASE("BadRevIDs", "[VersionVector]") {
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
        REQUIRE_FALSE(rev.tryParse(slice(kBadStrings[i]), true));
    }
    
    // Make sure we don't parse new-style IDs with the old parser:
    revidBuffer rev;
    REQUIRE_FALSE(rev.tryParse(slice("17@snej"), false));
}


TEST_CASE("Create", "[VersionVector]") {
    VersionVector v(slice("1@jens,2@bob"));
    REQUIRE(v[slice("jens")] == 1ull);
    REQUIRE(v[slice("bob")] == 2ull);
    REQUIRE(v[slice("may")] == 0ull);
    REQUIRE(v.current() == Version(1, slice("jens")));
    REQUIRE(v.count() == 2ul);

    // Convert to string and back:
    auto str = v.asString();
    REQUIRE(v.asString() == "1@jens,2@bob");
    VersionVector vv(str);
    REQUIRE(vv == v);
    REQUIRE(vv.asString() == v.asString());

    // Convert to Fleece and back:
    fleece::Encoder enc;
    enc << v;
    alloc_slice f = enc.extractOutput();
    REQUIRE(f.size == 22ul);
    auto fleeceRoot = fleece::Value::fromData(f);
    VersionVector vvf(fleeceRoot);
    REQUIRE(vvf == v);
}

TEST_CASE("CreateSingle", "[VersionVector]") {
    VersionVector v(slice("1@jens"));
    REQUIRE(v[slice("jens")] == 1ull);
    REQUIRE(v[slice("bob")] == 0ull);
    REQUIRE(v.current() == Version(1, slice("jens")));
    REQUIRE(v.count() == 1ul);
    REQUIRE(v.asString() == std::string("1@jens"));
}

TEST_CASE("Compare", "[VersionVector]") {
    VersionVector v(slice("1@jens,2@bob"));
    REQUIRE(v == v);
    REQUIRE_FALSE((v > v));
    REQUIRE_FALSE((v < v));
    REQUIRE(v.compareTo(v) == kSame);

    VersionVector oldv(slice("2@bob"));

    REQUIRE_FALSE((v == oldv));
    REQUIRE(v > oldv);
    REQUIRE(oldv < v);
    REQUIRE(v.compareTo(oldv) == kNewer);
    REQUIRE(oldv.compareTo(v) == kOlder);

    VersionVector otherV(slice("3@bob"));
    REQUIRE(v.compareTo(otherV) == kConflicting);
    REQUIRE(otherV.compareTo(v) == kConflicting);

    // Compare with single version:
    REQUIRE(v.compareTo(Version(slice("1@jens"))) == kSame);
    REQUIRE(v.compareTo(Version(slice("2@jens"))) == kOlder);
    REQUIRE(v.compareTo(Version(slice("1@bob"))) == kNewer);
    REQUIRE(v.compareTo(Version(slice("2@bob"))) == kNewer);
    REQUIRE(v.compareTo(Version(slice("3@bob"))) == kOlder);
    REQUIRE(v.compareTo(Version(slice("1@obo"))) == kOlder);
    REQUIRE(v >= Version(slice("1@bob")));
    REQUIRE(v >= Version(slice("2@bob")));
    REQUIRE_FALSE((v >= Version(slice("3@bob"))));

    REQUIRE(VersionVector(slice("1@*")).compareTo(VersionVector(slice("1@binky"))) == kConflicting);
}

TEST_CASE("Increment", "[VersionVector]") {
    VersionVector v(slice("123@jens,3141592654@bob"));
    v.incrementGen(slice("bob"));

    REQUIRE(v[slice("jens")] == 123ull);
    REQUIRE(v[slice("bob")] == 3141592655ull);
    REQUIRE(v.current() == Version(3141592655, slice("bob")));
    REQUIRE(v.count() == 2ul);

    auto str = v.asString();
    REQUIRE(str =="3141592655@bob,123@jens");

    v.incrementGen(slice("may"));

    REQUIRE(v[slice("jens")] == 123ull);
    REQUIRE(v[slice("bob")] == 3141592655ull);
    REQUIRE(v[slice("may")] == 1ull);
    REQUIRE(v.current() == Version(1, slice("may")));
    REQUIRE(v.count() == 3ul);

    str = v.asString();
    REQUIRE(str == "1@may,3141592655@bob,123@jens");
}

TEST_CASE("IncrementEmpty", "[VersionVector]") {
    VersionVector v;
    v.incrementGen(slice("may"));
    REQUIRE(v[slice("may")] == 1ull);
    REQUIRE(v.current() == Version(1, slice("may")));
    REQUIRE(v.count() == 1ul);
    REQUIRE(v.asString() == "1@may");
}

TEST_CASE("ImportExport", "[VersionVector]") {
    VersionVector v(slice("2@bob,1@*"));
    auto exported = v.exportAsString(slice("jens"));
    REQUIRE(exported == std::string("2@bob,1@jens"));

    VersionVector imported(exported);
    imported.compactMyPeerID(slice("jens"));
    REQUIRE(imported.asString() == std::string("2@bob,1@*"));
}


static void testMerge(const char *str1, const char *str2, const char *expectedStr) {
    VersionVector v1((slice(str1))), v2((slice(str2)));
    VersionVector result = v1.mergedWith(v2);
    auto resultStr = result.asString();
    REQUIRE(resultStr == (std::string)expectedStr);
}

TEST_CASE("Merge", "[VersionVector]") {
    testMerge("19@jens",             "1@bob",               "19@jens,1@bob");
    testMerge("19@jens",             "18@jens",             "19@jens");
    testMerge("18@jens",             "19@jens",             "19@jens");
    testMerge("18@jens,1@bob",       "19@jens",             "19@jens,1@bob");
    testMerge("19@jens,1@bob",       "2@bob,18@jens",       "19@jens,2@bob");
    testMerge("2@bob,18@jens",       "19@jens,1@bob",       "2@bob,19@jens");
    testMerge("19@jens,3@eve,1@bob", "2@bob,18@jens,3@eve", "19@jens,2@bob,3@eve");
    testMerge("2@bob,18@jens,3@eve", "19@jens,3@eve,1@bob", "2@bob,19@jens,3@eve");
}


static void testCanonicalString(const char *vecStr, const char *me, std::string expectedCanon) {
    VersionVector v {slice(vecStr)};
    REQUIRE(v.canonicalString(peerID(me)) == expectedCanon);
}

TEST_CASE("CanonicalString", "[VersionVector]") {
    testCanonicalString("19@bob",               "jens", "19@bob");
    testCanonicalString("2@bob,18@alice,3@eve", "jens", "18@alice,2@bob,3@eve");
    testCanonicalString("2@bob,18@*,3@eve",     "jens", "2@bob,3@eve,18@jens");
    testCanonicalString("2@bob,^deadbeef,3@eve","jens", "2@bob,^deadbeef,3@eve");
}


static void testMergedRevID(const char *vec1, const char *vec2, std::string expected) {
    VersionVector v1((slice(vec1))), v2((slice(vec2)));
    VersionVector result = v1.mergedWith(v2);
    result.insertMergeRevID(peerID("jens"), slice("{\"foo\":17}"));
    // NOTE: This assertion will fail if we ever change the algorithm for computing the digest:
    REQUIRE(result.asString() == expected);
}

TEST_CASE("MergedRevID", "[VersionVector]") {
    // Make sure the revID (first component, i.e. merge digest) is the same regardless of the
    // order of the merge:
    std::string digest = "^8GsuP45bb/QOE0QyQkM9Nlj0lTU=";
    testMergedRevID("2@bob,18@*,3@eve", "19@*,3@eve,1@bob",
                    digest + ",2@bob,19@*,3@eve");
    testMergedRevID("19@*,3@eve,1@bob", "2@bob,18@*,3@eve",
                    digest + ",19@*,2@bob,3@eve");
}
