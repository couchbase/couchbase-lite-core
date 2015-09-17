//
//  c4ViewTest.cc
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "C4View.h"


class C4ViewTest : public C4Test {
public:

    C4View *view;

    virtual void setUp() {
        C4Test::setUp();
        C4Error error;
        view = c4view_open(db, c4str("/tmp/forest_temp.view.index"), c4str("myview"), c4str("1"),
                           &error);
        Assert(view);
    }

    virtual void tearDown() {
        C4Error error;
        Assert(c4view_close(view, &error));
        C4Test::tearDown();
    }


    void testEmptyState() {
        AssertEqual(c4view_getTotalRows(view), 0ull);
        AssertEqual(c4view_getLastSequenceIndexed(view), 0ull);
        AssertEqual(c4view_getLastSequenceChangedAt(view), 0ull);
    }

    void testCreateIndex() {
        char docID[20];
        for (int i = 1; i <= 100; i++) {
            sprintf(docID, "doc-%03d", i);
            createRev(c4str(docID), kRevID, kBody);
        }

        C4Error error;
        C4Indexer* ind = c4indexer_begin(db, &view, 1, &error);
        Assert(ind);

        C4DocEnumerator* e = c4indexer_enumerateDocuments(ind, &error);
        Assert(e);

        C4Document *doc;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            // Index 'doc':
            C4Key *keys[2], *values[2];
            keys[0] = c4key_new();
            keys[1] = c4key_new();
            c4key_addString(keys[0], doc->docID);
            c4key_addNumber(keys[1], doc->sequence);
            values[0] = values[1] = NULL;
            Assert(c4indexer_emit(ind, doc, 0, 2, keys, values, &error));
            c4key_free(keys[0]);
            c4key_free(keys[1]);
        }
        AssertEqual(error.code, 0);

        AssertEqual(c4view_getTotalRows(view), 200ull);
        AssertEqual(c4view_getLastSequenceIndexed(view), 100ull);
        AssertEqual(c4view_getLastSequenceChangedAt(view), 100ull);
}

    CPPUNIT_TEST_SUITE( C4ViewTest );
    CPPUNIT_TEST( testEmptyState );
    CPPUNIT_TEST( testCreateIndex );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(C4ViewTest);
