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
#import "DocEnumerator.hh"

using namespace cbforest;
using namespace geohash;


static int numMapCalls;

static void updateIndex(Database *indexDB, MapReduceIndex* index) {
    MapReduceIndexer indexer;
    indexer.addIndex(index);
    auto seq = indexer.startingSequence();
    numMapCalls = 0;

    auto options = DocEnumerator::Options::kDefault;
    options.includeDeleted = true;
    DocEnumerator e(index->sourceStore(), seq, UINT64_MAX, options);
    while (e.next()) {
        auto &doc = e.doc();
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        if (!doc.deleted()) {
            // Here's the pseudo map function:
            ++numMapCalls;
            CollatableReader r(doc.body());
            geohash::area area;
            area.longitude.min = r.readDouble();
            area.latitude.min = r.readDouble();
            area.longitude.max = r.readDouble();
            area.latitude.max = r.readDouble();

            CollatableBuilder key;
            key.addGeoKey(slice("{\"geo\":true}"), area);
            CollatableBuilder value(numMapCalls);
            keys.push_back(key);
            values.push_back(value);
}
        indexer.emitDocIntoView(doc.key(), doc.sequence(), 0, keys, values);
    }
    indexer.finished();
}


@interface GeoIndex_Test : XCTestCase
@end

@implementation GeoIndex_Test
{
    std::string dbPath;
    Database *db;
    MapReduceIndex *index;
}


+ (void) initialize {
    if (self == [GeoIndex_Test class]) {
        LogLevel = kWarning;
    }
}

- (void) setUp {
    [super setUp];
    CreateTestDir();
    dbPath = PathForDatabaseNamed(@"geo_temp.fdb");
    db = new Database(dbPath, TestDBConfig());
    index = new MapReduceIndex(db, "geo", db);
}

- (void) tearDown {
    delete index;
    delete db;
    [super tearDown];
}

static double randomLat()   {return random() / (double)INT_MAX * 180.0 -  90.0;}
static double randomLon()   {return random() / (double)INT_MAX * 360.0 - 180.0;}

- (void) addCoords: (unsigned)n {
    NSLog(@"==== Adding %u docs...", n);
    srandom(42);
    Transaction t(db);
    IndexWriter writer(index, t);
    for (unsigned i = 0; i < n; ++i) {
        char docID[20];
        sprintf(docID, "%u", i);

        double lat0 = randomLat(), lon0 = randomLon();
        double lat1 = std::min(lat0 + 0.5, 90.0), lon1 = std::min(lon0 + 0.5, 180.0);
        CollatableBuilder body;
        body << lon0 << lat0 << lon1 << lat1;
        t.set(slice(docID), body);
        NSLog(@"Added %s --> (%+08.4f, %+09.4f)", docID, lat0, lon0);
    }
}

- (void) indexIt {
    index->setup(0, "1");
    NSLog(@"==== Indexing...");
    updateIndex(db, index);
}

- (void) testGeoIndex {
    [self addCoords: 100];
    auto queryArea = area(coord(10, 10), coord(40, 40));

    [self indexIt];

    NSLog(@"==== Querying...");
    unsigned found = 0;
    for (GeoIndexEnumerator e(index, queryArea); e.next(); ) {
        area a = e.keyBoundingBox();
        ++found;
        int64_t emitID = CollatableReader(e.value()).readInt();
        NSLog(@"key = %s = (%g, %g)...(%g, %g) doc = '%@' #%lld", e.key().toJSON().c_str(),
              a.latitude.min, a.longitude.min, a.latitude.max, a.longitude.max,
              (NSString*)e.docID(), emitID);
        XCTAssert(a.intersects(queryArea));
        NSString* geoJSON = (NSString*)e.keyGeoJSON();
        NSLog(@"keyGeoJSON = %@", geoJSON);
        AssertEqual(geoJSON, @"{\"geo\":true}");
    }
    NSLog(@"Found %u points in the query area", found);
}

@end
