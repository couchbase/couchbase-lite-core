//
//  c4ViewTest.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/16/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4View.h"
#include "c4DocEnumerator.h"
#include <iostream>


class C4ViewTest : public C4Test {
public:

    C4View *view {nullptr};

    C4ViewTest() {
        c4view_deleteByName(db, c4str("myview"), NULL);
        C4Error error;
        view = c4view_open(db, kC4SliceNull, c4str("myview"), c4str("1"),
                           c4db_getConfig(db), &error);
        REQUIRE(view);
    }

    ~C4ViewTest() {
        if (view) {
            C4Error error;
            bool ok = c4view_delete(view, &error);
            c4view_free(view);
            if (!ok) {
                char msg[256];
                fprintf(stderr, "ERROR: Failed to delete c4View: error %d/%d: %s\n",
                        error.domain, error.code, c4error_getMessageC(error, msg, sizeof(msg)));
                REQUIRE(false);
            }
        }
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
        REQUIRE(ind);

        C4DocEnumerator* e = c4indexer_enumerateDocuments(ind, &error);
        REQUIRE(e);

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
            REQUIRE(c4indexer_emit(ind, doc, 0, 2, keys, values, &error));
            c4key_free(keys[0]);
            c4key_free(keys[1]);
            c4doc_free(doc);
        }
        REQUIRE(error.code == 0);
        c4enum_free(e);
        REQUIRE(c4indexer_end(ind, true, &error));
    }

    void testDocPurge(bool compactAfterPurge);
    void createFullTextIndex(unsigned docCount);
};


TEST_CASE_METHOD(C4ViewTest, "View EmptyState", "[View][C]") {
    REQUIRE(c4view_getTotalRows(view) == (C4SequenceNumber)0);
    REQUIRE(c4view_getLastSequenceIndexed(view) == (C4SequenceNumber)0);
    REQUIRE(c4view_getLastSequenceChangedAt(view) == (C4SequenceNumber)0);
}

TEST_CASE_METHOD(C4ViewTest, "View CreateIndex", "[View][C]") {
    createIndex();

    REQUIRE(c4view_getTotalRows(view) == (C4SequenceNumber)200);
    REQUIRE(c4view_getLastSequenceIndexed(view) == (C4SequenceNumber)100);
    REQUIRE(c4view_getLastSequenceChangedAt(view) == (C4SequenceNumber)100);
}

TEST_CASE_METHOD(C4ViewTest, "View QueryIndex", "[View][C]") {
    createIndex();

    C4Error error;
    auto e = c4view_query(view, NULL, &error);
    REQUIRE(e);

    int i = 0;
    while (c4queryenum_next(e, &error)) {
        ++i;
        //std::cerr << "Key: " << toJSON(e->key) << "  Value: " << toJSON(e->value) << "\n";
        char buf[20];
        if (i <= 100) {
            sprintf(buf, "%d", i);
            REQUIRE(e->docSequence == (C4SequenceNumber)i);
        } else {
            sprintf(buf, "\"doc-%03d\"", i - 100);
            REQUIRE(e->docSequence == (C4SequenceNumber)(i - 100));
        }
        REQUIRE(toJSON(e->key) == std::string(buf));
        REQUIRE(e->value == c4str("1234"));

    }
    c4queryenum_free(e);
    REQUIRE(error.code == 0);
    REQUIRE(i == 200);
}

TEST_CASE_METHOD(C4ViewTest, "View IndexVersion", "[View][C]") {
    createIndex();

    // Reopen view with same version string:
    C4Error error;
    REQUIRE(c4view_close(view, &error));
    c4view_free(view);

    view = c4view_open(db, kC4SliceNull, c4str("myview"), c4str("1"),
                       c4db_getConfig(db), &error);
    REQUIRE(view != NULL);

    REQUIRE(c4view_getTotalRows(view) == (C4SequenceNumber)200);
    REQUIRE(c4view_getLastSequenceIndexed(view) == (C4SequenceNumber)100);
    REQUIRE(c4view_getLastSequenceChangedAt(view) == (C4SequenceNumber)100);

    // Reopen view with different version string:
    REQUIRE(c4view_close(view, &error));
    c4view_free(view);

    view = c4view_open(db, kC4SliceNull, c4str("myview"), c4str("2"),
                       c4db_getConfig(db), &error);
    REQUIRE(view != NULL);

    REQUIRE(c4view_getTotalRows(view) == (C4SequenceNumber)0);
    REQUIRE(c4view_getLastSequenceIndexed(view) == (C4SequenceNumber)0);
    REQUIRE(c4view_getLastSequenceChangedAt(view) == (C4SequenceNumber)0);
}

void C4ViewTest::testDocPurge(bool compactAfterPurge) {
    createIndex();
    auto lastIndexed = c4view_getLastSequenceIndexed(view);
    auto lastSeq = c4db_getLastSequence(db);
    REQUIRE(lastIndexed == lastSeq);

    // Purge one of the indexed docs:
    C4Error err;
    {
        TransactionHelper t(db);
        REQUIRE(c4db_purgeDoc(db, c4str("doc-023"), &err));
    }

    if (compactAfterPurge)
        REQUIRE(c4db_compact(db, &err));

    // ForestDB assigns sequences to deletions, so the purge bumped the db's sequence,
    // invalidating the view index:
    lastIndexed = c4view_getLastSequenceIndexed(view);
    lastSeq = c4db_getLastSequence(db);
    REQUIRE(lastIndexed < lastSeq);

    updateIndex();

    // Verify that the purged doc is no longer in the index:
    C4Error error;
    auto e = c4view_query(view, NULL, &error);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
        ++i;
    }
    c4queryenum_free(e);
    REQUIRE(i == 198); // 2 rows of doc-023 are gone
}

TEST_CASE_METHOD(C4ViewTest, "View DocPurge", "[View][C]")             {testDocPurge(false);}
TEST_CASE_METHOD(C4ViewTest, "View DocPurgeWithCompact", "[View][C]")  {testDocPurge(true);}


void C4ViewTest::createFullTextIndex(unsigned docCount) {
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
    REQUIRE(ind);

    C4DocEnumerator* e = c4indexer_enumerateDocuments(ind, &error);
    REQUIRE(e);

    C4Document *doc;
    while (NULL != (doc = c4enum_nextDocument(e, &error))) {
        // Index 'doc':
        C4Key *keys[1];
        C4Slice values[1];
        keys[0] = c4key_newFullTextString(doc->selectedRev.body, c4str("en"));
        values[0] = c4str("1234");
        REQUIRE(c4indexer_emit(ind, doc, 0, 1, keys, values, &error));
        c4key_free(keys[0]);
        c4doc_free(doc);
    }
    REQUIRE(error.code == 0);
    c4enum_free(e);
    REQUIRE(c4indexer_end(ind, true, &error));
}

TEST_CASE_METHOD(C4ViewTest, "View CreateFullTextIndex", "[View][C]") {
    createFullTextIndex(100);
}

TEST_CASE_METHOD(C4ViewTest, "View QueryFullTextIndex", "[View][C]") {
    createFullTextIndex(3);

    // Search for "somewhere":
    C4Error error;
    C4QueryEnumerator* e = c4view_fullTextQuery(view, c4str("somewhere"), kC4SliceNull,
                                                NULL, &error);
    REQUIRE(e);
    REQUIRE(c4queryenum_next(e, &error));
    REQUIRE(e->docID == c4str("doc-001"));
    REQUIRE(e->docSequence == 1uLL);
    REQUIRE(e->fullTextTermCount == 1u);
    REQUIRE(e->fullTextTerms[0].termIndex == 0u);
    REQUIRE(e->fullTextTerms[0].start == 8u);
    REQUIRE(e->fullTextTerms[0].length == 9u);

    REQUIRE(!c4queryenum_next(e, &error));
    REQUIRE(error.code == 0);
    c4queryenum_free(e);

    // Search for "cat":
    e = c4view_fullTextQuery(view, c4str("cat"), kC4SliceNull,  NULL, &error);
    REQUIRE(e);
    int i = 0;
    while (c4queryenum_next(e, &error)) {
        ++i;
        REQUIRE(e->fullTextTermCount == 1u);
        REQUIRE(e->fullTextTerms[0].termIndex == 0u);
        if (e->docSequence == 1) {
            REQUIRE(e->fullTextTerms[0].start == 20u);
            REQUIRE(e->fullTextTerms[0].length == 4u);
        } else {
            REQUIRE(e->docSequence == 3uLL);
            REQUIRE(e->fullTextTerms[0].start == 4u);
            REQUIRE(e->fullTextTerms[0].length == 3u);
        }
    }
    c4queryenum_free(e);
    REQUIRE(error.code == 0);
    REQUIRE(i == 2);

    // Search for "cat bark":
    e = c4view_fullTextQuery(view, c4str("cat bark"), kC4SliceNull,  NULL, &error);
    REQUIRE(c4queryenum_next(e, &error));
    REQUIRE(e->docID == c4str("doc-001"));
    REQUIRE(e->docSequence == 1uLL);
    REQUIRE(e->fullTextTermCount == 2u);
    REQUIRE(e->fullTextTerms[0].termIndex == 0u);
    REQUIRE(e->fullTextTerms[0].start == 20u);
    REQUIRE(e->fullTextTerms[0].length == 4u);
    REQUIRE(e->fullTextTerms[1].termIndex == 1u);
    REQUIRE(e->fullTextTerms[1].start == 29u);
    REQUIRE(e->fullTextTerms[1].length == 7u);

    REQUIRE(!c4queryenum_next(e, &error));
    REQUIRE(error.code == 0);
    c4queryenum_free(e);
}


/*
class C4SQLiteViewTest : public C4ViewTest {

    virtual const char* storageType() const override     {return kC4SQLiteStorageEngine;}

    CPPUNIT_TEST_SUB_SUITE( C4SQLiteViewTest, C4ViewTest );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(C4SQLiteViewTest);
*/
