//
//  GeoHash_Test.m
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/3/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//

#include "Geohash.hh"

#include "LiteCoreTest.hh"


static void verify_hash(double lat, double lon, int len, const char* expected)
{
    geohash::hash result(geohash::coord(lat, lon), len);
    REQUIRE(std::string(result.string) == std::string(expected));
}


TEST_CASE("Geohash Encode", "[geohash]") {
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
    REQUIRE(area.latitude.max == Approx(lat_max));
    REQUIRE(area.latitude.min == Approx(lat_min));
    REQUIRE(area.longitude.max == Approx(lon_max));
    REQUIRE(area.longitude.min == Approx(lon_min));
}


TEST_CASE("Geohash Decode", "[geohash]") {
    verify_area("c216ne", 45.3680419921875, -121.70654296875, 45.37353515625, -121.695556640625);
    verify_area("C216Ne", 45.3680419921875, -121.70654296875, 45.37353515625, -121.695556640625);
    verify_area("dqcw4", 39.0234375, -76.552734375, 39.0673828125, -76.5087890625);
    verify_area("DQCW4", 39.0234375, -76.552734375, 39.0673828125, -76.5087890625);
}


TEST_CASE("Geohash Verification", "[geohash]") {
    REQUIRE(geohash::hash("dqcw5").isValid());
    REQUIRE(geohash::hash("dqcw7").isValid());
    REQUIRE_FALSE(geohash::hash("abcwd").isValid());
    REQUIRE_FALSE(geohash::hash("dqcw5@").isValid());
}


TEST_CASE("Geohash DistanceTo", "[geohash]") {
    // See http://www.distance.to/New-York/San-Francisco
    static const double kMilesPerKm = 0.62137;
    const geohash::coord sf (37.774929,-122.419418);
    const geohash::coord nyc(40.714268, -74.005974);
    REQUIRE(sf.distanceTo(nyc) == Approx(2566 / kMilesPerKm).epsilon(0.5));
    REQUIRE(sf.distanceTo(sf) == Approx(0));

    auto h = sf.encodeWithKmAccuracy(0.1);
    REQUIRE(string(h.string) == "9q8yyk8");
    h = nyc.encodeWithKmAccuracy(0.01);
    REQUIRE(string(h.string) == "dr5regy3z");
}


static void verify_adjacent(const char *origin, geohash::direction dir, const char *expected)
{
    geohash::hash destination = geohash::hash(origin).adjacent(dir);
    REQUIRE(std::string(destination.string) == expected);
}


TEST_CASE("Geohash Adjacent", "[geohash]") {
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

    REQUIRE(std::string(north.string) ==      hash1);
    REQUIRE(std::string(south.string) ==     hash2);
    REQUIRE(std::string(west.string) ==      hash3);
    REQUIRE(std::string(east.string) ==      hash4);
    REQUIRE(std::string(north.adjacent(geohash::WEST).string) ==hash5);
    REQUIRE(std::string(north.adjacent(geohash::EAST).string) ==hash6);
    REQUIRE(std::string(south.adjacent(geohash::WEST).string) ==hash7);
    REQUIRE(std::string(south.adjacent(geohash::EAST).string) ==hash8);
}


TEST_CASE("Geohash Neighbors", "[geohash]") {
    verify_neighbors("dqcw5", "dqcw7", "dqctg", "dqcw4", "dqcwh", "dqcw6", "dqcwk", "dqctf", "dqctu");
    verify_neighbors("xn774c", "xn774f", "xn774b", "xn7749", "xn7751", "xn774d", "xn7754", "xn7748", "xn7750");
    verify_neighbors("gcpuvpk", "gcpuvps", "gcpuvph", "gcpuvp7", "gcpuvpm", "gcpuvpe", "gcpuvpt", "gcpuvp5", "gcpuvpj");
    verify_neighbors("c23nb62w", "c23nb62x", "c23nb62t", "c23nb62q", "c23nb62y", "c23nb62r", "c23nb62z", "c23nb62m", "c23nb62v");
}


TEST_CASE("Geohash Covering", "[geohash]") {
    geohash::area box(geohash::coord(45, -121), geohash::coord(46, -120));
    std::vector<geohash::hashRange> hashes = box.coveringHashRanges(10);
    std::sort(hashes.begin(), hashes.end());
    Debug("Covering hashes:");
    for (auto i = hashes.begin(); i != hashes.end(); ++i) {
        if (i->count == 1) {
            Debug("    %s", i->first().string);
        } else {
            Debug("    %s ... %s (%u)", i->first().string, i->last().string, i->count);
        }
    }

    REQUIRE(hashes.size() == 7ul);
    REQUIRE(std::string(hashes[0].first().string) =="c21b");  REQUIRE(hashes[0].count == 2u);
    REQUIRE(std::string(hashes[1].first().string) =="c21f");  REQUIRE(hashes[1].count == 2u);
    REQUIRE(std::string(hashes[2].first().string) =="c21u");  REQUIRE(hashes[2].count == 2u);
    REQUIRE(std::string(hashes[3].first().string) =="c240");  REQUIRE(hashes[3].count == 10u);
    REQUIRE(std::string(hashes[4].first().string) =="c24d");  REQUIRE(hashes[4].count == 2u);
    REQUIRE(std::string(hashes[5].first().string) =="c24h");  REQUIRE(hashes[5].count == 4u);
    REQUIRE(std::string(hashes[6].first().string) =="c24s");  REQUIRE(hashes[6].count == 2u);
}


TEST_CASE("Geohash Covering2", "[geohash]") {
    geohash::area box(geohash::coord(10, 10), geohash::coord(20, 20));
    std::vector<geohash::hashRange> hashes = box.coveringHashRanges(10);
    std::sort(hashes.begin(), hashes.end());
    Debug("Covering hashes:");
    for (auto i = hashes.begin(); i != hashes.end(); ++i) {
        if (i->count == 1) {
            Debug("    %s", i->first().string);
        } else {
            Debug("    %s ... %s (%u)", i->first().string, i->last().string, i->count);
        }

        geohash::area a = i->first().decode();
        Debug("        (%g, %g)...(%g, %g)",
              a.latitude.min,a.longitude.min, a.latitude.max,a.longitude.max);
    }
    REQUIRE(hashes.size() == 2ul);
    REQUIRE(std::string(hashes[0].first().string) =="s1");  REQUIRE(hashes[0].count == 1u);
    REQUIRE(std::string(hashes[1].first().string) =="s3");  REQUIRE(hashes[1].count == 5u);
}
