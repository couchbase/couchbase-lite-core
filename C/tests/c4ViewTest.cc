//
//  c4ViewTest.cc
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4View.h"
#include <iostream>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifdef _MSC_VER
static const char *kViewIndexPath = "C:\\tmp\\forest_temp.view.index";
#else
static const char *kViewIndexPath = "/tmp/forest_temp.view.index";
#endif


class C4ViewTest : public C4Test {
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
        if (view)
            Assert(c4view_delete(view, &error));
        C4Test::tearDown();
    }


    void testEmptyState() {
        AssertEqual(c4view_getTotalRows(view), (C4SequenceNumber)0);
        AssertEqual(c4view_getLastSequenceIndexed(view), (C4SequenceNumber)0);
        AssertEqual(c4view_getLastSequenceChangedAt(view), (C4SequenceNumber)0);
    }

    void createIndex() {
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
            C4Key *keys[2];
            C4Slice values[2];
            keys[0] = c4key_new();
            keys[1] = c4key_new();
            c4key_addString(keys[0], doc->docID);
            c4key_addNumber(keys[1], doc->sequence);
            values[0] = values[1] = c4str("1234");
            Assert(c4indexer_emit(ind, doc, 0, 2, keys, values, &error));
            c4key_free(keys[0]);
            c4key_free(keys[1]);
        }
        AssertEqual(error.code, 0);
        Assert(c4indexer_end(ind, true, &error));
    }

    void testCreateIndex() {
        createIndex();

        AssertEqual(c4view_getTotalRows(view), (C4SequenceNumber)200);
        AssertEqual(c4view_getLastSequenceIndexed(view), (C4SequenceNumber)100);
        AssertEqual(c4view_getLastSequenceChangedAt(view), (C4SequenceNumber)100);
    }

    void testQueryIndex() {
        createIndex();

        C4Error error;
        auto e = c4view_query(view, NULL, &error);
        Assert(e);

        int i = 0;
        while (c4queryenum_next(e, &error)) {
            ++i;
            //std::cerr << "Key: " << toJSON(e->key) << "  Value: " << toJSON(e->value) << "\n";
            char buf[20];
            if (i <= 100) {
                sprintf(buf, "%d", i);
                AssertEqual(e->docSequence, (C4SequenceNumber)i);
            } else {
                sprintf(buf, "\"doc-%03d\"", i - 100);
                AssertEqual(e->docSequence, (C4SequenceNumber)(i - 100));
            }
            AssertEqual(toJSON(e->key), std::string(buf));
            AssertEqual(e->value, c4str("1234"));

        }
        AssertEqual(error.code, 0);
        AssertEqual(i, 200);
    }

    void testIndexVersion() {
        createIndex();

        // Reopen view with same version string:
        C4Error error;
        Assert(c4view_close(view, &error));
        view = c4view_open(db, c4str(kViewIndexPath), c4str("myview"), c4str("1"),
                           kC4DB_Create, encryptionKey(), &error);
        Assert(view != NULL);

        AssertEqual(c4view_getTotalRows(view), (C4SequenceNumber)200);
        AssertEqual(c4view_getLastSequenceIndexed(view), (C4SequenceNumber)100);
        AssertEqual(c4view_getLastSequenceChangedAt(view), (C4SequenceNumber)100);

        // Reopen view with different version string:
        Assert(c4view_close(view, &error));
        view = c4view_open(db, c4str(kViewIndexPath), c4str("myview"), c4str("2"),
                           kC4DB_Create, encryptionKey(), &error);
        Assert(view != NULL);

        AssertEqual(c4view_getTotalRows(view), (C4SequenceNumber)0);
        AssertEqual(c4view_getLastSequenceIndexed(view), (C4SequenceNumber)0);
        AssertEqual(c4view_getLastSequenceChangedAt(view), (C4SequenceNumber)0);
    }



    CPPUNIT_TEST_SUITE( C4ViewTest );
    CPPUNIT_TEST( testEmptyState );
    CPPUNIT_TEST( testCreateIndex );
    CPPUNIT_TEST( testQueryIndex );
    CPPUNIT_TEST( testIndexVersion );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(C4ViewTest);
