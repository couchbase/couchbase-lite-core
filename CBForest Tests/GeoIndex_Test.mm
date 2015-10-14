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


class TestGeoMappable : public Mappable {
public:
    TestGeoMappable(const Document& doc)
    :Mappable(doc)
    {
        CollatableReader r(doc.body());
        area.longitude.min = r.readDouble();
        area.latitude.min = r.readDouble();
        area.longitude.max = r.readDouble();
        area.latitude.max = r.readDouble();
    }

    geohash::area area;
};

class TestGeoMapFn : public MapFn {
public:
    static int numMapCalls;
    virtual void operator() (const Mappable& mappable, EmitFn& emit) {
        ++numMapCalls;
        auto area = ((TestGeoMappable&)mappable).area;
        Collatable value(numMapCalls);
        emit(area, slice("{\"geo\":true}"), value);
    }
};

int TestGeoMapFn::numMapCalls;


class TestGeoIndexer : public MapReduceIndexer {
public:
    static bool updateIndex(Database* database, MapReduceIndex* index) {
        TestGeoIndexer indexer;
        indexer.addIndex(index, new Transaction(database));
        return indexer.run();
    }

    virtual void addDocument(const Document& doc) {
        TestGeoMappable mappable(doc);
        addMappable(mappable);
    }
};


@interface GeoIndex_Test : XCTestCase
@end

@implementation GeoIndex_Test
{
    std::string dbPath;
    Database *db;
    KeyStore source;
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
    source = (KeyStore)*db;
    index = new MapReduceIndex(db, "geo", source);
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
        Collatable body;
        body << lon0 << lat0 << lon1 << lat1;
        t.set(slice(docID), body);
        NSLog(@"Added %s --> (%+08.4f, %+09.4f)", docID, lat0, lon0);
    }
}

- (void) indexIt {
    {
        Transaction trans(db);
        index->setup(trans, 0, new TestGeoMapFn, "1");
    }
    NSLog(@"==== Indexing...");
    XCTAssertTrue(TestGeoIndexer::updateIndex(db, index));
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
