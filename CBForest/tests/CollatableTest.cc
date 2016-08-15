//
//  CollatableTest.cc
//  CBForest
//
//  Created by Jens Alfke on 8/2/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Collatable.hh"
#include "CBForestTest.hh"

using namespace cbforest;


template <typename T>
static alloc_slice collatableData(T val) {
    CollatableBuilder c(val);
    return c.extractOutput();
}

template <typename T>
static int sgn(T n) {return n<0 ? -1 : (n>0 ? 1 : 0);}

template <typename T1, typename T2>
static int compareCollated(T1 obj1, T2 obj2) {
    CollatableBuilder c1, c2;
    c1 << obj1;
    c2 << obj2;
    return sgn(cbforest::slice(c1).compare(cbforest::slice(c2)));
}

static uint64_t randn(uint64_t limit) {
    uint64_t n;
    randomBytes(slice(&n, sizeof(n)));
    return n % limit;
}

static double randf() {
    union {double d; struct {uint32_t u1, u2;};} n;
    do {
        n.u1 = (uint32_t)random();
        n.u2 = (uint32_t)random();
    } while (isnan(n.d) || isinf(n.d));
    return n.d;
}

template <typename T>
static CollatableReader roundTrip(T input) {
    static alloc_slice sLastEncoded;
    CollatableBuilder c(input);
    sLastEncoded = c.extractOutput();
    return CollatableReader(sLastEncoded);
}

static void checkRoundTrip(std::string str) {
    AssertEqual(roundTrip(str).readString(), alloc_slice(str));
}

static void compareNumber(double n1, double n2) {
    AssertEqual(compareCollated(n1, n2), sgn(n1 - n2));
}

static void assertJSON(const CollatableBuilder &c, const char *json) {
    AssertEqual(c.toJSON(), std::string(json));
}



class CollatableTest : public CppUnit::TestFixture {

void testScalars() {
    AssertEqual(compareCollated(true, false), 1);
    AssertEqual(compareCollated(true, false), 1);
    AssertEqual(compareCollated(true, 17), -1);
    AssertEqual(compareCollated(1, 1), 0);
    AssertEqual(compareCollated(123, 1), 1);
    AssertEqual(compareCollated(0x100, 0xFF), 1);
    AssertEqual(compareCollated(0x1234, 0x12), 1);
    AssertEqual(compareCollated(0x1234, 0x13), 1);
    AssertEqual(compareCollated((INT64_MAX), (INT32_MAX)), 1);

    AssertEqual(compareCollated((-1), (0)), -1);
    AssertEqual(compareCollated((-1), (1)), -1);
    AssertEqual(compareCollated((-123), (-7)), -1);
}

void testRandomNumbers() {
    for (int i=0; i< 10000; i++) {
        int64_t n1, n2;
        n1 = (int64_t)randn(UINT64_MAX) >> randn(63);
        n2 = (int64_t)randn(UINT64_MAX) >> randn(63);
        compareNumber(n1, n2);
    }
}

void testFloats() {
    double numbers[] = {0, 1, 2, 10, 32, 63, 64, 256, M_PI, 100, 6.02e23, 6.02e-23, 0.01,
        DBL_MAX, DBL_MIN,
        M_PI + 0.1, M_PI - 0.1,
        -1, -64, -M_PI, -6.02e23};
    const int nFloats = sizeof(numbers)/sizeof(numbers[0]);
    for (int i=0; i<nFloats; i++) {
        double n1 = numbers[i];
        //Log("%16g --> %s\n", numbers[i], collatableData(n1).hexString().c_str());
        AssertEqual(roundTrip(n1).readDouble(), n1);
        for (int j=0; j<nFloats; j++) {
            compareNumber(n1, numbers[j]);
        }
    }
}

void testRandomFloats() {
    srandomdev();
    for (int i=0; i< 100000; i++) {
        double n1 = randf(), n2 = randf();
        //Log("Compare: %s <--> %s", n1, n2);
        AssertEqual(roundTrip(n1).readDouble(), n1);
        AssertEqual(roundTrip(n2).readDouble(), n2);
        compareNumber(n1, n2);
    }
}

void testRoundTripInts() {
    uint64_t n = 1;
    for (int bits = 0; bits < 63; ++bits, n<<=1) {
        CollatableBuilder c;
        c << n - 1;
        alloc_slice encoded((cbforest::slice)c);
        CollatableReader reader(encoded);
        uint64_t result = reader.readInt();
        //Log("2^%2d - 1: %llx --> %llx", bits, n-1, result);
        // At 2^54-1 floating-point roundoff starts to occur. This is known, so skip the assert
        if (bits < 54)
            AssertEqual(result, n-1);
    }
}

void testStrings() {
    AssertEqual(compareCollated((std::string)"", 7), 1);
    AssertEqual(compareCollated((std::string)"", (std::string)""), 0);
    AssertEqual(compareCollated((std::string)"", true), 1);
    AssertEqual(compareCollated((std::string)"", (std::string)" "), -1);
    AssertEqual(compareCollated((std::string)"~", (std::string)"a"), -1);
    AssertEqual(compareCollated((std::string)"A", (std::string)"a"), 1);
    AssertEqual(compareCollated((std::string)"\n", (std::string)" "), -1);
    AssertEqual(compareCollated((std::string)"Hello world", (std::string)""), 1);
    AssertEqual(compareCollated((std::string)"Hello world", (std::string)"Aaron"), 1);
    AssertEqual(compareCollated((std::string)"Hello world", (std::string)"Hello world!"), -1);
    AssertEqual(compareCollated((std::string)"hello World", (std::string)"hellO wOrLd"), -1); // uppercase letters win ties
    AssertEqual(compareCollated((std::string)"Hello world", (std::string)"jello world"), -1); // but letter order comes first
    AssertEqual(compareCollated((std::string)"hello world", (std::string)"Jello world"), -1);

    // Non-ASCII characters aren't going to sort according to the Unicode Collation Algorithm,
    // but they should still sort after all ASCII characters.
    AssertEqual(compareCollated((std::string)"Hello world", (std::string)"Hello wörld!"), -1);

    // Make sure nulls and control characters don't break anything:
    checkRoundTrip("foo\0");
    checkRoundTrip("foo\0\1\2bar");
    checkRoundTrip("\033\034\035");
    // DEL is weird. There isn't room in the Collatable encoding to give it a unique value, so it
    // gets the same value as space, meaning it decodes to space.
    AssertEqual(roundTrip("hey\177there").readString(), alloc_slice("hey there"));
}

void testIndexKey() {
    std::string key = "OR";
    CollatableBuilder collKey;
    collKey << key;

    std::string docID = "foo";
    CollatableBuilder collatableDocID;
    collatableDocID << docID;

    CollatableBuilder indexKey;
    indexKey.beginArray();
    indexKey << collKey << collatableDocID << (int64_t)1234;
    indexKey.endArray();

    alloc_slice encoded((cbforest::slice)indexKey);

    CollatableReader reader(encoded);
    reader.beginArray();
    cbforest::slice readKey = reader.read();
    Assert(readKey == (cbforest::slice)collKey);
    alloc_slice readDocID = reader.readString();
    Assert(readDocID == (cbforest::slice)docID);
    int64_t readSequence = reader.readInt();
    AssertEqual(readSequence, 1234ll);
}

void testDump() {
    CollatableBuilder("howdy");
    CollatableBuilder c;
    c.addBool(false);
    assertJSON(c, "false");

    c = CollatableBuilder();
    c.addBool(true);
    assertJSON(c, "true");

    assertJSON(CollatableBuilder(66), "66");

    assertJSON(CollatableBuilder("howdy"), "\"howdy\"");
    assertJSON(CollatableBuilder("\"ironic\""), "\"\\\"ironic\\\"\"");
    assertJSON(CollatableBuilder("an \"ironic\" twist"), "\"an \\\"ironic\\\" twist\"");
    assertJSON(CollatableBuilder("\\foo\\"), "\"\\\\foo\\\\\"");
    assertJSON(CollatableBuilder("\tline1\nline2\t"), "\"\\tline1\\nline2\\t\"");
    assertJSON(CollatableBuilder("line1\01\02line2"), "\"line1\\u0001\\u0002line2\"");

    c = CollatableBuilder();
    c.beginArray();
    c << 1234;
    c.endArray();
    assertJSON(c, "[1234]");

    c = CollatableBuilder();
    c.beginArray();
    c << 1234;
    c << 5678;
    c.endArray();
    assertJSON(c, "[1234,5678]");

    c = CollatableBuilder();
    c.beginMap();
    c << "name";
    c << "Frank";
    c << "age";
    c << 11;
    c.endMap();
    assertJSON(c, "{\"name\":\"Frank\",\"age\":11}");
}


    CPPUNIT_TEST_SUITE( CollatableTest );
    CPPUNIT_TEST( testScalars );
    CPPUNIT_TEST( testRandomNumbers );
    CPPUNIT_TEST( testFloats );
    CPPUNIT_TEST( testRandomFloats );
    CPPUNIT_TEST( testRoundTripInts );
    CPPUNIT_TEST( testStrings );
    CPPUNIT_TEST( testIndexKey );
    CPPUNIT_TEST( testDump );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(CollatableTest);
