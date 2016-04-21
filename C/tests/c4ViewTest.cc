//
//  c4ViewTest.cc
//  CBForest
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4View.h"
#include "c4DocEnumerator.h"
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
        if (view) {
            C4Error error;
            bool ok = c4view_delete(view, &error);
            c4view_free(view);
            if (!ok) {
                fprintf(stderr, "ERROR: Failed to delete c4View: error %d/%d\n", error.domain, error.code);
                Assert(false);
            }
        }
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
        updateIndex();
    }

    void updateIndex() {
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
            c4doc_free(doc);
        }
        AssertEqual(error.code, 0);
        c4enum_free(e);
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
        c4queryenum_free(e);
        AssertEqual(error.code, 0);
        AssertEqual(i, 200);
    }

    void testIndexVersion() {
        createIndex();

        // Reopen view with same version string:
        C4Error error;
        Assert(c4view_close(view, &error));
        c4view_free(view);

        view = c4view_open(db, c4str(kViewIndexPath), c4str("myview"), c4str("1"),
                           kC4DB_Create, encryptionKey(), &error);
        Assert(view != NULL);

        AssertEqual(c4view_getTotalRows(view), (C4SequenceNumber)200);
        AssertEqual(c4view_getLastSequenceIndexed(view), (C4SequenceNumber)100);
        AssertEqual(c4view_getLastSequenceChangedAt(view), (C4SequenceNumber)100);

        // Reopen view with different version string:
        Assert(c4view_close(view, &error));
        c4view_free(view);

        view = c4view_open(db, c4str(kViewIndexPath), c4str("myview"), c4str("2"),
                           kC4DB_Create, encryptionKey(), &error);
        Assert(view != NULL);

        AssertEqual(c4view_getTotalRows(view), (C4SequenceNumber)0);
        AssertEqual(c4view_getLastSequenceIndexed(view), (C4SequenceNumber)0);
        AssertEqual(c4view_getLastSequenceChangedAt(view), (C4SequenceNumber)0);
    }

    void testDocPurge()             {testDocPurge(false);}
    void testDocPurgeWithCompact()  {testDocPurge(true);}

    void testDocPurge(bool compactAfterPurge) {
        createIndex();
        auto lastIndexed = c4view_getLastSequenceIndexed(view);
        auto lastSeq = c4db_getLastSequence(db);
        AssertEqual(lastIndexed, lastSeq);

        // Purge one of the indexed docs:
        C4Error err;
        {
            TransactionHelper t(db);
            Assert(c4db_purgeDoc(db, c4str("doc-023"), &err));
        }

        if (compactAfterPurge)
            Assert(c4db_compact(db, &err));

        // ForestDB assigns sequences to deletions, so the purge bumped the db's sequence,
        // invalidating the view index:
        lastIndexed = c4view_getLastSequenceIndexed(view);
        lastSeq = c4db_getLastSequence(db);
        Assert(lastIndexed < lastSeq);

        updateIndex();

        // Verify that the purged doc is no longer in the index:
        C4Error error;
        auto e = c4view_query(view, NULL, &error);
        Assert(e);
        int i = 0;
        while (c4queryenum_next(e, &error)) {
            ++i;
        }
        c4queryenum_free(e);
        AssertEqual(i, 198); // 2 rows of doc-023 are gone
    }

    void createFullTextIndex(unsigned docCount) {
        char docID[20];
        for (unsigned i = 1; i <= docCount; i++) {
            sprintf(docID, "doc-%03d", i);
            const char *body = nullptr;
            switch (i % 3) {
                case 0: body = "The cat sat on the mat."; break;
                case 1: body = "Outside SomeWhere a cät was barking"; break;
                case 2: body = "The bark of a tree is rough?"; break;
            }
            createRev(c4str(docID), kRevID, c4str(body));
        }

        C4Error error;
        C4Indexer* ind = c4indexer_begin(db, &view, 1, &error);
        Assert(ind);

        C4DocEnumerator* e = c4indexer_enumerateDocuments(ind, &error);
        Assert(e);

        C4Document *doc;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            // Index 'doc':
            C4Key *keys[1];
            C4Slice values[1];
            keys[0] = c4key_newFullTextString(doc->selectedRev.body, c4str("en"));
            values[0] = c4str("1234");
            Assert(c4indexer_emit(ind, doc, 0, 1, keys, values, &error));
            c4key_free(keys[0]);
            c4doc_free(doc);
        }
        AssertEqual(error.code, 0);
        c4enum_free(e);
        Assert(c4indexer_end(ind, true, &error));
    }

    void testCreateFullTextIndex() {
        createFullTextIndex(100);
    }

    void testQueryFullTextIndex() {
        createFullTextIndex(3);

        // Search for "somewhere":
        C4Error error;
        C4QueryEnumerator* e = c4view_fullTextQuery(view, c4str("somewhere"), kC4SliceNull,
                                                    NULL, &error);
        Assert(e);
        Assert(c4queryenum_next(e, &error));
        AssertEqual(e->docID, c4str("doc-001"));
        AssertEqual(e->docSequence, 1uLL);
        AssertEqual(e->fullTextTermCount, 1u);
        AssertEqual(e->fullTextTerms[0].termIndex, 0u);
        AssertEqual(e->fullTextTerms[0].start, 8u);
        AssertEqual(e->fullTextTerms[0].length, 9u);

        Assert(!c4queryenum_next(e, &error));
        AssertEqual(error.code, 0);
        c4queryenum_free(e);

        // Search for "cat":
        e = c4view_fullTextQuery(view, c4str("cat"), kC4SliceNull,  NULL, &error);
        Assert(e);
        int i = 0;
        while (c4queryenum_next(e, &error)) {
            ++i;
            AssertEqual(e->fullTextTermCount, 1u);
            AssertEqual(e->fullTextTerms[0].termIndex, 0u);
            if (e->docSequence == 1) {
                AssertEqual(e->fullTextTerms[0].start, 20u);
                AssertEqual(e->fullTextTerms[0].length, 4u);
            } else {
                AssertEqual(e->docSequence, 3uLL);
                AssertEqual(e->fullTextTerms[0].start, 4u);
                AssertEqual(e->fullTextTerms[0].length, 3u);
            }
        }
        c4queryenum_free(e);
        AssertEqual(error.code, 0);
        AssertEqual(i, 2);

        // Search for "cat bark":
        e = c4view_fullTextQuery(view, c4str("cat bark"), kC4SliceNull,  NULL, &error);
        Assert(c4queryenum_next(e, &error));
        AssertEqual(e->docID, c4str("doc-001"));
        AssertEqual(e->docSequence, 1uLL);
        AssertEqual(e->fullTextTermCount, 2u);
        AssertEqual(e->fullTextTerms[0].termIndex, 0u);
        AssertEqual(e->fullTextTerms[0].start, 20u);
        AssertEqual(e->fullTextTerms[0].length, 4u);
        AssertEqual(e->fullTextTerms[1].termIndex, 1u);
        AssertEqual(e->fullTextTerms[1].start, 29u);
        AssertEqual(e->fullTextTerms[1].length, 7u);

        Assert(!c4queryenum_next(e, &error));
        AssertEqual(error.code, 0);
        c4queryenum_free(e);
    }


    CPPUNIT_TEST_SUITE( C4ViewTest );
    CPPUNIT_TEST( testEmptyState );
    CPPUNIT_TEST( testCreateIndex );
    CPPUNIT_TEST( testQueryIndex );
    CPPUNIT_TEST( testIndexVersion );
    CPPUNIT_TEST( testDocPurge );
    CPPUNIT_TEST( testDocPurgeWithCompact );
    CPPUNIT_TEST( testCreateFullTextIndex );
    CPPUNIT_TEST( testQueryFullTextIndex );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(C4ViewTest);
