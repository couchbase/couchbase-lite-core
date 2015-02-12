//
//  GeoIndex_Test.m
//  CBForest
//
//  Created by Jens Alfke on 11/7/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#import <XCTest/XCTest.h>
#import "testutil.h"
#import "GeoIndex.hh"

using namespace forestdb;
using namespace geohash;


static NSString* kDBPathStr;
static std::string kDBPath;


@interface GeoIndex_Test : XCTestCase
@end

@implementation GeoIndex_Test
{
    Database* database;
    Index *index;
}


+ (void) initialize {
    if (self == [GeoIndex_Test class]) {
        LogLevel = kWarning;
        kDBPathStr = [NSTemporaryDirectory() stringByAppendingPathComponent: @"forest_temp.fdb"];
        kDBPath = kDBPathStr.fileSystemRepresentation;
    }
}

- (void) setUp {
    NSError* error;
    [[NSFileManager defaultManager] removeItemAtPath: kDBPathStr error: &error];
    database = new Database(kDBPath, Database::defaultConfig());
    index = new Index(database, "geo");
}

- (void) tearDown {
    delete index;
    delete database;
    [super tearDown];
}

static double randomLat()   {return random() / (double)INT_MAX * 180.0 -  90.0;}
static double randomLon()   {return random() / (double)INT_MAX * 360.0 - 180.0;}

- (void) addCoords: (unsigned)n {
    srandom(42);
    uint64_t rowCount;
    Transaction t(database);
    IndexWriter writer(index, t);
    for (unsigned i = 0; i < n; ++i) {
        char docID[20];
        sprintf(docID, "%u", i);
        coord c(randomLat(), randomLon());
        std::vector<Collatable> keys, values;
        keys.push_back(Collatable(c));
        values.push_back(Collatable(i));
        writer.update(slice(docID), i+1, keys, values, rowCount);
        //NSLog(@"Added (%g, %g)", c.latitude, c.longitude);
    }
}

- (void) testGeoIndex {
    [self addCoords: 10000];
    auto queryArea = area(coord(10, 10), coord(20, 20));

    unsigned found = 0, inArea = 0;
    for (IndexEnumerator e(index,
                           Collatable(""), slice::null,
                           Collatable("Z"), slice::null,
                           DocEnumerator::Options::kDefault); e.next(); ) {
        CollatableReader reader(e.key());
        geohash::hash hash(reader.readString());
        area a = hash.decode();
        ++found;
        if (queryArea.intersects(a)) {
            inArea++;
            NSLog(@"key = %s = (%g, %g)...(%g, %g)", e.key().dump().c_str(),
                  a.latitude.min, a.longitude.min, a.latitude.max, a.longitude.max);
        }
    }
    AssertEq(found, 10000u);
    NSLog(@"Found %u points in the query area", inArea);

//    LogLevel = kDebug;
    NSLog(@"Querying...");
    found = 0;
    for (GeoIndexEnumerator e(index, queryArea); e.next(); ) {
        auto keyCoord = e.keyCoord();
        ++found;
        NSLog(@"Found (%g, %g)", keyCoord.latitude, keyCoord.longitude);
    }
    AssertEq(found, inArea);
}

@end
