//
//  GeoHash_Test.m
//  CBForest
//
//  Created by Jens Alfke on 11/3/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "cgeohash.h"


#define AssertEqualCStrings(STR1, STR2) XCTAssertTrue(strcmp((STR1),(STR2)) == 0)


@interface GeoHash_Test : XCTestCase
@end

@implementation GeoHash_Test


static void verify_hash(id self, double lat, double lon, int len, const char* expected)
{
    char *hash;
    hash = GEOHASH_encode(lat, lon, len);
    AssertEqualCStrings(hash, expected);
    free(hash);
    /*CU_ASSERT_PTR_NULL();*/
}

- (void) testEncode
{
    verify_hash(self,       45.37,       -121.7,  6, "c216ne"        );
    verify_hash(self,  47.6062095, -122.3320708, 13, "c23nb62w20sth" );
    verify_hash(self,  35.6894875,  139.6917064, 13, "xn774c06kdtve" );
    verify_hash(self, -33.8671390,  151.2071140, 13, "r3gx2f9tt5sne" );
    verify_hash(self,  51.5001524,   -0.1262362, 13, "gcpuvpk44kprq" );
}

static void verify_area(
                 id self,
                 const char *hash,
                 double lat_min, double lon_min,
                 double lat_max, double lon_max)
{
    GEOHASH_area *area;
    area = GEOHASH_decode(hash);
    XCTAssertEqualWithAccuracy(area->latitude.max,   lat_max, 0.001);
    XCTAssertEqualWithAccuracy(area->latitude.min,   lat_min, 0.001);
    XCTAssertEqualWithAccuracy(area->longitude.max, lon_max, 0.001);
    XCTAssertEqualWithAccuracy(area->longitude.min, lon_min, 0.001);
    GEOHASH_free_area(area);
}

- (void) testDecode
{
    verify_area(self, "c216ne", 45.3680419921875, -121.70654296875, 45.37353515625, -121.695556640625);
    verify_area(self, "C216Ne", 45.3680419921875, -121.70654296875, 45.37353515625, -121.695556640625);
    verify_area(self, "dqcw4", 39.0234375, -76.552734375, 39.0673828125, -76.5087890625);
    verify_area(self, "DQCW4", 39.0234375, -76.552734375, 39.0673828125, -76.5087890625);
}

static void verify_adjacent(id self, const char *origin, GEOHASH_direction dir, const char *expected)
{
    char *hash;
    hash = GEOHASH_get_adjacent(origin, dir);
    AssertEqualCStrings(hash, expected);
    free(hash);
}

- (void) testAdjacent
{
    verify_adjacent(self, "dqcjq", GEOHASH_NORTH, "dqcjw");
    verify_adjacent(self, "dqcjq", GEOHASH_SOUTH, "dqcjn");
    verify_adjacent(self, "dqcjq", GEOHASH_WEST,  "dqcjm");
    verify_adjacent(self, "dqcjq", GEOHASH_EAST,  "dqcjr");
}

static void verify_neighbors(
                      id self,
                      const char *origin,
                      const char *hash1,
                      const char *hash2,
                      const char *hash3,
                      const char *hash4,
                      const char *hash5,
                      const char *hash6,
                      const char *hash7,
                      const char *hash8
                      )
{
    GEOHASH_neighbors *neighbors;
    neighbors = GEOHASH_get_neighbors(origin);

    AssertEqualCStrings(neighbors->north,      hash1);
    AssertEqualCStrings(neighbors->south,      hash2);
    AssertEqualCStrings(neighbors->west,       hash3);
    AssertEqualCStrings(neighbors->east,       hash4);
    AssertEqualCStrings(neighbors->north_west, hash5);
    AssertEqualCStrings(neighbors->north_east, hash6);
    AssertEqualCStrings(neighbors->south_west, hash7);
    AssertEqualCStrings(neighbors->south_east, hash8);

    GEOHASH_free_neighbors(neighbors);
}

- (void) testNeighbors
{
    verify_neighbors(self, "dqcw5", "dqcw7", "dqctg", "dqcw4", "dqcwh", "dqcw6", "dqcwk", "dqctf", "dqctu");
    verify_neighbors(self, "xn774c", "xn774f", "xn774b", "xn7749", "xn7751", "xn774d", "xn7754", "xn7748", "xn7750");
    verify_neighbors(self, "gcpuvpk", "gcpuvps", "gcpuvph", "gcpuvp7", "gcpuvpm", "gcpuvpe", "gcpuvpt", "gcpuvp5", "gcpuvpj");
    verify_neighbors(self, "c23nb62w", "c23nb62x", "c23nb62t", "c23nb62q", "c23nb62y", "c23nb62r", "c23nb62z", "c23nb62m", "c23nb62v");
}

- (void) testVerification
{
    XCTAssertEqual(GEOHASH_verify_hash("dqcw5"), 1);
    XCTAssertEqual(GEOHASH_verify_hash("dqcw7"), 1);
    XCTAssertEqual(GEOHASH_verify_hash("abcwd"), 0);
    XCTAssertEqual(GEOHASH_verify_hash("dqcw5@"), 0);
}


@end
