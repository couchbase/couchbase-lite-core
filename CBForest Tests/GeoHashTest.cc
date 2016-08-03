//
//  GeoHash_Test.m
//  CBForest
//
//  Created by Jens Alfke on 11/3/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "Geohash.hh"

#include "CBForestTest.hh"


class GeoHashTest : public DatabaseTestFixture {


static void verify_hash(double lat, double lon, int len, const char* expected)
{
    geohash::hash result(geohash::coord(lat, lon), len);
    AssertEqualCStrings(result.string, expected);
}

void testEncode() {
    verify_hash(      45.37,       -121.7,  6, "c216ne"        );
    verify_hash( 47.6062095, -122.3320708, 13, "c23nb62w20sth" );
    verify_hash( 35.6894875,  139.6917064, 13, "xn774c06kdtve" );
    verify_hash(-33.8671390,  151.2071140, 13, "r3gx2f9tt5sne" );
    verify_hash( 51.5001524,   -0.1262362, 13, "gcpuvpk44kprq" );
}

static void verify_area(const char *hash,
                        double lat_min, double lon_min,
                        double lat_max, double lon_max)
{
    geohash::area area = geohash::hash(hash).decode();
    AssertEqualWithAccuracy(area.latitude.max,   lat_max, 0.001);
    AssertEqualWithAccuracy(area.latitude.min,   lat_min, 0.001);
    AssertEqualWithAccuracy(area.longitude.max,  lon_max, 0.001);
    AssertEqualWithAccuracy(area.longitude.min,  lon_min, 0.001);
}

void testDecode() {
    verify_area("c216ne", 45.3680419921875, -121.70654296875, 45.37353515625, -121.695556640625);
    verify_area("C216Ne", 45.3680419921875, -121.70654296875, 45.37353515625, -121.695556640625);
    verify_area("dqcw4", 39.0234375, -76.552734375, 39.0673828125, -76.5087890625);
    verify_area("DQCW4", 39.0234375, -76.552734375, 39.0673828125, -76.5087890625);
}

void testVerification() {
    Assert(geohash::hash("dqcw5").isValid());
    Assert(geohash::hash("dqcw7").isValid());
    Assert(!geohash::hash("abcwd").isValid());
    Assert(!geohash::hash("dqcw5@").isValid());
}

void testDistanceTo() {
    // See http://www.distance.to/New-York/San-Francisco
    static const double kMilesPerKm = 0.62137;
    const geohash::coord sf (37.774929,-122.419418);
    const geohash::coord nyc(40.714268, -74.005974);
    AssertEqualWithAccuracy(sf.distanceTo(nyc), 2566 / kMilesPerKm, 1.0);
    AssertEqualWithAccuracy(sf.distanceTo(sf), 0, 0.01);

    auto h = sf.encodeWithKmAccuracy(0.1);
    AssertEqualCStrings(h.string, "9q8yyk8");
    h = nyc.encodeWithKmAccuracy(0.01);
    AssertEqualCStrings(h.string, "dr5regy3z");
}


static void verify_adjacent(const char *origin, geohash::direction dir, const char *expected)
{
    geohash::hash destination = geohash::hash(origin).adjacent(dir);
    AssertEqualCStrings(destination.string, expected);
}

void testAdjacent() {
    verify_adjacent("dqcjq", geohash::NORTH, "dqcjw");
    verify_adjacent("dqcjq", geohash::SOUTH, "dqcjn");
    verify_adjacent("dqcjq", geohash::WEST,  "dqcjm");
    verify_adjacent("dqcjq", geohash::EAST,  "dqcjr");
}

static void verify_neighbors(const char *originStr,
                             const char *hash1,
                             const char *hash2,
                             const char *hash3,
                             const char *hash4,
                             const char *hash5,
                             const char *hash6,
                             const char *hash7,
                             const char *hash8)
{
    geohash::hash origin = geohash::hash(originStr);
    geohash::hash north = origin.adjacent(geohash::NORTH);
    geohash::hash south = origin.adjacent(geohash::SOUTH);
    geohash::hash east = origin.adjacent(geohash::EAST);
    geohash::hash west = origin.adjacent(geohash::WEST);

    AssertEqualCStrings(north.string,      hash1);
    AssertEqualCStrings(south.string,      hash2);
    AssertEqualCStrings(west.string,       hash3);
    AssertEqualCStrings(east.string,       hash4);
    AssertEqualCStrings(north.adjacent(geohash::WEST).string, hash5);
    AssertEqualCStrings(north.adjacent(geohash::EAST).string, hash6);
    AssertEqualCStrings(south.adjacent(geohash::WEST).string, hash7);
    AssertEqualCStrings(south.adjacent(geohash::EAST).string, hash8);
}

void testNeighbors() {
    verify_neighbors("dqcw5", "dqcw7", "dqctg", "dqcw4", "dqcwh", "dqcw6", "dqcwk", "dqctf", "dqctu");
    verify_neighbors("xn774c", "xn774f", "xn774b", "xn7749", "xn7751", "xn774d", "xn7754", "xn7748", "xn7750");
    verify_neighbors("gcpuvpk", "gcpuvps", "gcpuvph", "gcpuvp7", "gcpuvpm", "gcpuvpe", "gcpuvpt", "gcpuvp5", "gcpuvpj");
    verify_neighbors("c23nb62w", "c23nb62x", "c23nb62t", "c23nb62q", "c23nb62y", "c23nb62r", "c23nb62z", "c23nb62m", "c23nb62v");
}

void testCovering() {
    geohash::area box(geohash::coord(45, -121), geohash::coord(46, -120));
    std::vector<geohash::hashRange> hashes = box.coveringHashRanges(10);
    std::sort(hashes.begin(), hashes.end());
    Log("Covering hashes:");
    for (auto i = hashes.begin(); i != hashes.end(); ++i)
        if (i->count == 1)
            Log("    %s", i->first().string);
        else
            Log("    %s ... %s (%u)", i->first().string, i->last().string, i->count);
    AssertEqual(hashes.size(), 7ul);
    AssertEqualCStrings(hashes[0].first().string, "c21b");  AssertEqual(hashes[0].count, 2u);
    AssertEqualCStrings(hashes[1].first().string, "c21f");  AssertEqual(hashes[1].count, 2u);
    AssertEqualCStrings(hashes[2].first().string, "c21u");  AssertEqual(hashes[2].count, 2u);
    AssertEqualCStrings(hashes[3].first().string, "c240");  AssertEqual(hashes[3].count, 10u);
    AssertEqualCStrings(hashes[4].first().string, "c24d");  AssertEqual(hashes[4].count, 2u);
    AssertEqualCStrings(hashes[5].first().string, "c24h");  AssertEqual(hashes[5].count, 4u);
    AssertEqualCStrings(hashes[6].first().string, "c24s");  AssertEqual(hashes[6].count, 2u);
}

void testCovering2() {
    geohash::area box(geohash::coord(10, 10), geohash::coord(20, 20));
    std::vector<geohash::hashRange> hashes = box.coveringHashRanges(10);
    std::sort(hashes.begin(), hashes.end());
    Log("Covering hashes:");
    for (auto i = hashes.begin(); i != hashes.end(); ++i) {
        if (i->count == 1)
            Log("    %s", i->first().string);
        else
            Log("    %s ... %s (%u)", i->first().string, i->last().string, i->count);
        geohash::area a = i->first().decode();
        Log("        (%g, %g)...(%g, %g)",
              a.latitude.min,a.longitude.min, a.latitude.max,a.longitude.max);
    }
    AssertEqual(hashes.size(), 2ul);
    AssertEqualCStrings(hashes[0].first().string, "s1");  AssertEqual(hashes[0].count, 1u);
    AssertEqualCStrings(hashes[1].first().string, "s3");  AssertEqual(hashes[1].count, 5u);
}

    CPPUNIT_TEST_SUITE( GeoHashTest );
    CPPUNIT_TEST( testEncode );
    CPPUNIT_TEST( testDecode );
    CPPUNIT_TEST( testVerification );
    CPPUNIT_TEST( testDistanceTo );
    CPPUNIT_TEST( testAdjacent );
    CPPUNIT_TEST( testNeighbors );
    CPPUNIT_TEST( testCovering );
    CPPUNIT_TEST( testCovering2 );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(GeoHashTest);
