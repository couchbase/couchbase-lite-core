//
//  GeoIndex_Test.m
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 11/7/14.
//  Copyright (c) 2014-2016 Couchbase. All rights reserved.
//

#include "GeoIndex.hh"
#include "RecordEnumerator.hh"
#include "LiteCoreTest.hh"
#include "PlatformCompat.hh"

using namespace geohash;


static int numMapCalls;

static void updateIndex(DataFile *indexDB, MapReduceIndex& index) {
    MapReduceIndexer indexer;
    indexer.addIndex(index);
    auto seq = indexer.startingSequence();
    numMapCalls = 0;

    RecordEnumerator::Options options;
    options.includeDeleted = true;
    RecordEnumerator e(index.sourceStore(), seq, UINT64_MAX, options);
    while (e.next()) {
        auto &rec = e.record();
        std::vector<Collatable> keys;
        std::vector<alloc_slice> values;
        if (!rec.deleted()) {
            // Here's the pseudo map function:
            ++numMapCalls;
            CollatableReader r(rec.body());
            geohash::area area;
            area.longitude.min = r.readDouble();
            area.latitude.min = r.readDouble();
            area.longitude.max = r.readDouble();
            area.latitude.max = r.readDouble();

            CollatableBuilder key;
            key.addGeoKey("{\"geo\":true}"_sl, area);
            CollatableBuilder value(numMapCalls);
            keys.push_back(key);
            values.push_back(value);
}
        indexer.emitDocIntoView(rec.key(), rec.sequence(), 0, keys, values);
    }
    indexer.finished();
}


class GeoIndexTest : public DataFileTestFixture {
public:
    MapReduceIndex *index;

    GeoIndexTest(int testOption)
    :DataFileTestFixture(testOption)
    {
        index = new MapReduceIndex(db->getKeyStore("geo"), *db);
    }

    ~GeoIndexTest() {
        delete index;
    }

    static double randomLat()   {return random() / (double)INT_MAX * 180.0 -  90.0;}
    static double randomLon()   {return random() / (double)INT_MAX * 360.0 - 180.0;}

    void addCoords(unsigned n) {
        Debug("==== Adding %u records...", n);
        srandom(42);
        Transaction t(db);
        IndexWriter writer(*index, t);
        for (unsigned i = 0; i < n; ++i) {
            char recordID[20];
            sprintf(recordID, "%u", i);

            double lat0 = randomLat(), lon0 = randomLon();
            double lat1 = std::min(lat0 + 0.5, 90.0), lon1 = std::min(lon0 + 0.5, 180.0);
            CollatableBuilder body;
            body << lon0 << lat0 << lon1 << lat1;
            store->set(slice(recordID), body, t);
            Debug("Added %s --> (%+08.4f, %+09.4f)", recordID, lat0, lon0);
        }
        t.commit();
    }
    
    void indexIt() {
        index->setup(0, "1");
        Debug("==== Indexing...");
        updateIndex(db, *index);
    }
    
};


N_WAY_TEST_CASE_METHOD(GeoIndexTest, "GeoIndex", "[GeoIndex],[Index]") {
    addCoords(100);
    auto queryArea = area(coord(10, 10), coord(40, 40));

    indexIt();

    Debug("==== Querying...");
    unsigned found = 0;
    for (GeoIndexEnumerator e(*index, queryArea); e.next(); ) {
        area a = e.keyBoundingBox();
        ++found;
        unsigned emitID = e.geoID();
        Debug("key = %s = (%g, %g)...(%g, %g) rec = '%s' #%u", e.key().toJSON().c_str(),
              a.latitude.min, a.longitude.min, a.latitude.max, a.longitude.max,
              e.recordID().cString(), emitID);
        REQUIRE(a.intersects(queryArea));
        auto geoJSON = e.keyGeoJSON();
        Debug("keyGeoJSON = %s", geoJSON.cString());
        REQUIRE(geoJSON.asString() == string("{\"geo\":true}"));
    }
    Debug("Found %u points in the query area", found);
}
