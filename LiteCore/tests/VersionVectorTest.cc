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
    revidBuffer rev("1-f0f0"_sl);
    REQUIRE((std::string)rev == std::string("1-f0f0"));
    static const uint8_t expectedBytes[] = {0x01, 0xf0, 0xf0};
    REQUIRE(rev == slice(expectedBytes, sizeof(expectedBytes)));

    rev = stringToRev("1234-1234567890abcdef1234567890abcdef");
    REQUIRE((std::string)rev == std::string("1234-1234567890abcdef1234567890abcdef"));
    static const uint8_t expectedBytes2[18] = {0xd2, 0x09, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD,
        0xEF, 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB, 0xCD, 0xEF};
    REQUIRE(rev == slice(expectedBytes2, sizeof(expectedBytes2)));

    // New-style ('clock') revID:
    rev.parseNew("17@snej"_sl);
    REQUIRE(rev.isClock());
    REQUIRE(rev.generation() == 17u);
    REQUIRE(rev.digest() == "snej"_sl);
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
    REQUIRE_FALSE(rev.tryParse("17@snej"_sl, false));
}


TEST_CASE("Create", "[VersionVector]") {
    VersionVector v("1@jens,2@bob"_sl);
    REQUIRE(v["jens"_sl] == 1ull);
    REQUIRE(v["bob"_sl] == 2ull);
    REQUIRE(v["may"_sl] == 0ull);
    REQUIRE(v.current() == Version(1, "jens"_sl));
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
    VersionVector v("1@jens"_sl);
    REQUIRE(v["jens"_sl] == 1ull);
    REQUIRE(v["bob"_sl] == 0ull);
    REQUIRE(v.current() == Version(1, "jens"_sl));
    REQUIRE(v.count() == 1ul);
    REQUIRE(v.asString() == std::string("1@jens"));
}

TEST_CASE("Compare", "[VersionVector]") {
    VersionVector v("1@jens,2@bob"_sl);
    REQUIRE(v == v);
    REQUIRE_FALSE((v > v));
    REQUIRE_FALSE((v < v));
    REQUIRE(v.compareTo(v) == kSame);

    VersionVector oldv("2@bob"_sl);

    REQUIRE_FALSE((v == oldv));
    REQUIRE(v > oldv);
    REQUIRE(oldv < v);
    REQUIRE(v.compareTo(oldv) == kNewer);
    REQUIRE(oldv.compareTo(v) == kOlder);

    VersionVector otherV("3@bob"_sl);
    REQUIRE(v.compareTo(otherV) == kConflicting);
    REQUIRE(otherV.compareTo(v) == kConflicting);

    // Compare with single version:
    REQUIRE(v.compareTo(Version("1@jens"_sl)) == kSame);
    REQUIRE(v.compareTo(Version("2@jens"_sl)) == kOlder);
    REQUIRE(v.compareTo(Version("1@bob"_sl)) == kNewer);
    REQUIRE(v.compareTo(Version("2@bob"_sl)) == kNewer);
    REQUIRE(v.compareTo(Version("3@bob"_sl)) == kOlder);
    REQUIRE(v.compareTo(Version("1@obo"_sl)) == kOlder);
    REQUIRE(v >= Version("1@bob"_sl));
    REQUIRE(v >= Version("2@bob"_sl));
    REQUIRE_FALSE((v >= Version("3@bob"_sl)));

    REQUIRE(VersionVector("1@*"_sl).compareTo(VersionVector("1@binky"_sl)) == kConflicting);
}

TEST_CASE("Increment", "[VersionVector]") {
    VersionVector v("123@jens,3141592654@bob"_sl);
    v.incrementGen("bob"_sl);

    REQUIRE(v["jens"_sl] == 123ull);
    REQUIRE(v["bob"_sl] == 3141592655ull);
    REQUIRE(v.current() == Version(3141592655, "bob"_sl));
    REQUIRE(v.count() == 2ul);

    auto str = v.asString();
    REQUIRE(str =="3141592655@bob,123@jens");

    v.incrementGen("may"_sl);

    REQUIRE(v["jens"_sl] == 123ull);
    REQUIRE(v["bob"_sl] == 3141592655ull);
    REQUIRE(v["may"_sl] == 1ull);
    REQUIRE(v.current() == Version(1, "may"_sl));
    REQUIRE(v.count() == 3ul);

    str = v.asString();
    REQUIRE(str == "1@may,3141592655@bob,123@jens");
}

TEST_CASE("IncrementEmpty", "[VersionVector]") {
    VersionVector v;
    v.incrementGen("may"_sl);
    REQUIRE(v["may"_sl] == 1ull);
    REQUIRE(v.current() == Version(1, "may"_sl));
    REQUIRE(v.count() == 1ul);
    REQUIRE(v.asString() == "1@may");
}

TEST_CASE("ImportExport", "[VersionVector]") {
    VersionVector v("2@bob,1@*"_sl);
    auto exported = v.exportAsString("jens"_sl);
    REQUIRE(exported == std::string("2@bob,1@jens"));

    VersionVector imported(exported);
    imported.compactMyPeerID("jens"_sl);
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
    result.insertMergeRevID(peerID("jens"), "{\"foo\":17}"_sl);
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
