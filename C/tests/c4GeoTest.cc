//
//  c4GeoTest.cc
//  CBForest
//
//  Created by Jens Alfke on 1/5/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4View.h"
#include "c4DocEnumerator.h"
#include <iostream>
#ifndef _MSC_VER
#include <unistd.h>
#else
#define random() rand()
#define srandom(seed) srand(seed)
#include <algorithm>
#endif

#ifdef _MSC_VER
static const char *kViewIndexPath = "C:\\tmp\\forest_temp.view.index";
#else
static const char *kViewIndexPath = "/tmp/forest_temp.view.index";
#endif

static double randomLat() { return random() / (double)RAND_MAX * 180.0 - 90.0; }
static double randomLon() { return random() / (double)RAND_MAX * 360.0 - 180.0; }

class C4GeoTest : public C4Test {
public:

    C4View *view;

    virtual void setUp() {
        C4Test::setUp();
        ::unlink(kViewIndexPath);
        C4Error error;
        view = c4view_open(db, c4str(kViewIndexPath), c4str("myview"), c4str("1"),
                           kC4DB_Create, encryptionKey(), &error);
        Assert(view);
    }

    virtual void tearDown() {
        C4Error error;
        if (view && !c4view_delete(view, &error)) {
            fprintf(stderr, "ERROR: Failed to delete c4View: error %d/%d\n", error.domain, error.code);
            Assert(false);
        }
        c4view_free(view);
        C4Test::tearDown();
    }


    void createDocs(unsigned n, bool verbose =false) {
        srandom(42);
        TransactionHelper t(db);

        for (unsigned i = 0; i < n; ++i) {
            char docID[20];
            sprintf(docID, "%u", i);

            double lat0 = randomLat(), lon0 = randomLon();
            double lat1 = std::min(lat0 + 0.5, 90.0), lon1 = std::min(lon0 + 0.5, 180.0);
            char body[1000];
            sprintf(body, "(%g, %g, %g, %g)", lon0, lat0, lon1, lat1);

            C4DocPutRequest rq = {};
            rq.docID = c4str(docID);
            rq.body = c4str(body);
            rq.save = true;
            C4Error error;
            C4Document *doc = c4doc_put(db, &rq, NULL, &error);
            Assert(doc != NULL);
            if (verbose)
                fprintf(stderr, "Added %s --> %s\n", docID, body);
            c4doc_free(doc);
        }
    }

    void createIndex() {
        C4Error error;
        C4Indexer* ind = c4indexer_begin(db, &view, 1, &error);
        Assert(ind);

        C4DocEnumerator* e = c4indexer_enumerateDocuments(ind, &error);
        Assert(e);

        C4Document *doc;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            char body [1000];
            memcpy(body, doc->selectedRev.body.buf, doc->selectedRev.body.size);
            body[doc->selectedRev.body.size] = '\0';

            C4GeoArea area;
            AssertEqual(sscanf(body, "(%lf, %lf, %lf, %lf)",
                               &area.xmin, &area.ymin, &area.xmax, &area.ymax),
                        4);

            C4Key *keys[1];
            C4Slice values[1];
            keys[0] = c4key_newGeoJSON(c4str("{\"geo\":true}"), area);
            values[0] = c4str("1234");
            Assert(c4indexer_emit(ind, doc, 0, 1, keys, values, &error));
            c4key_free(keys[0]);
            c4doc_free(doc);
        }
        c4enum_free(e);
        AssertEqual(error.code, 0);
        Assert(c4indexer_end(ind, true, &error));
    }

    void testCreateIndex() {
        createDocs(100);
        createIndex();
    }

    void testQuery() {
        static const bool verbose = false;
        createDocs(100, verbose);
        createIndex();

        C4GeoArea queryArea = {10, 10, 40, 40};
        C4Error error;
        C4QueryEnumerator* e = c4view_geoQuery(view, queryArea, &error);
        Assert(e);

        unsigned found = 0;
        while (c4queryenum_next(e, &error)) {
            ++found;
            C4GeoArea a = e->geoBBox;
            if (verbose) {
                fprintf(stderr, "Found doc %.*s : (%g, %g)--(%g, %g)\n",
                    (int)e->docID.size, e->docID.buf, a.xmin, a.ymin, a.xmax, a.ymax);
            }

            C4Slice expected = C4STR("1234");
            AssertEqual(e->value, expected);
            Assert(a.xmin <= 40 && a.xmax >= 10 && a.ymin <= 40 && a.ymax >= 10);

            expected = C4STR("{\"geo\":true}");
            AssertEqual(e->geoJSON, expected);
        }
        c4queryenum_free(e);
        AssertEqual(error.code, 0);
        AssertEqual(found, 2u);
    }


    CPPUNIT_TEST_SUITE( C4GeoTest );
    CPPUNIT_TEST( testCreateIndex );
    CPPUNIT_TEST( testQuery );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(C4GeoTest);
