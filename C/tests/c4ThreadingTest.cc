//
//  c4ThreadingTest.cpp
//  CBForest
//
//  Created by Jens Alfke on 7/1/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4View.h"
#include "c4DocEnumerator.h"
#include <chrono>
#include <iostream>
#include <thread>
#ifndef _MSC_VER
#include <unistd.h>
#endif


#ifdef _MSC_VER
static const char *kDBPath = "C:\\tmp\\forest_temp.fdb";
static const char *kViewIndexPath = "C:\\tmp\\forest_temp.view.index";
#else
static const char *kDBPath = "/tmp/forest_temp.fdb";
static const char *kViewIndexPath = "/tmp/forest_temp.view.index";
#endif


class C4ThreadingTest : public C4Test {
public:

    static const bool kLog = false;
    static const int kNumDocs = 10000;

    static const bool kSharedHandle = false; // Use same C4Database on all threads?
    

    C4View* v {nullptr};

    virtual void setUp() {
        C4Test::setUp();
        ::unlink(kViewIndexPath);
        v = openView(db);
    }

    virtual void tearDown() {
        closeView(v);
        C4Test::tearDown();
    }


    C4Database* openDB() {
        C4Database* database = c4db_open(c4str(kDBPath), kC4DB_Create, encryptionKey(), NULL);
        Assert(database);
        return database;
    }

    C4View* openView(C4Database* onDB) {
        C4View* view = c4view_open(onDB, c4str(kViewIndexPath), c4str("myview"), c4str("1"),
                                   kC4DB_Create, encryptionKey(), NULL);
        Assert(view);
        return view;
    }

    void closeView(C4View* view) {
        c4view_close(view, NULL);
        c4view_free(view);
    }

    void closeDB(C4Database* database) {
        c4db_close(database, NULL);
        c4db_free(database);
    }


    void testCreateVsEnumerate() {
        std::cerr << "\nThreading test ";

        std::thread thread1([this]{addDocsTask();});
        std::thread thread2([this]{updateIndexTask();});
        std::thread thread3([this]{queryIndexTask();});
//        std::thread thread4([this]{enumDocsTask();});

        thread1.join();
        thread2.join();
        thread3.join();
//        thread4.join();
        std::cerr << "Threading test done!\n";
    }


    void addDocsTask() {
        // This implicitly uses the 'db' connection created (but not used) by the main thread
        if (kLog) fprintf(stderr, "Adding documents...\n");
        for (int i = 1; i <= kNumDocs; i++) {
            if (kLog) fprintf(stderr, "(%d) ", i); else if (i%10 == 0) fprintf(stderr, ":");
            char docID[20];
            sprintf(docID, "doc-%05d", i);
            createRev(c4str(docID), kRevID, kBody);
            //std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }


    void enumDocsTask() {
        C4Database* database = db;// openDB();

        if (kLog) fprintf(stderr, "Enumerating documents...\n");
        int n;
        int i = 0;
        do {
            if (kLog) fprintf(stderr, "\nEnumeration #%3d: ", ++i);

            (void)c4db_getLastSequence(database);

            C4Error error;
            C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
            options.flags |= kC4IncludeBodies;
            auto e = c4db_enumerateChanges(database, 0, &options, &error);
            Assert(e);

            n = 0;
            while (c4enum_next(e, &error)) {
                auto doc = c4enum_getDocument(e, &error);
                Assert(doc);
                c4doc_free(doc);
                n++;
            }
            AssertEqual(error.code, 0);
            c4enum_free(e);
            if (kLog) fprintf(stderr, "-- %d docs", n);
        } while (n < kNumDocs);
    }


    void updateIndexTask() {
        C4Database* database = kSharedHandle ? db : openDB();
        C4View* view = kSharedHandle ? v : openView(database);

        int i = 0;
        do {
            if (kLog) fprintf(stderr, "\nIndex update #%3d: ", ++i);
            updateIndex(database, view);
            std::this_thread::sleep_for(std::chrono::microseconds(700));
        } while (c4view_getLastSequenceIndexed(view) < kNumDocs);

        if (!kSharedHandle) {
            closeView(view);
            closeDB(database);
        }
    }

    void updateIndex(C4Database* updateDB, C4View* view) {
        C4Error error;
        C4Indexer* ind = c4indexer_begin(updateDB, &view, 1, &error);
        Assert(ind);

        C4DocEnumerator* e = c4indexer_enumerateDocuments(ind, &error);
        if (!e) {
            c4indexer_end(ind, false, NULL);
            Assert(error.code == 0);
            return;
        }

        if (kLog) fprintf(stderr, "<< ");

        C4Document *doc;
        C4SequenceNumber lastSeq = 0;
        while (NULL != (doc = c4enum_nextDocument(e, &error))) {
            // Index 'doc':
            if (kLog) fprintf(stderr, "(#%lld) ", doc->sequence);
            if (lastSeq)
                AssertEqual(doc->sequence, lastSeq+1);
            lastSeq = doc->sequence;
            C4Key *keys[1];
            C4Slice values[1];
            keys[0] = c4key_new();
            c4key_addString(keys[0], doc->docID);
            values[0] = c4str("1234");
            Assert(c4indexer_emit(ind, doc, 0, 1/*2*/, keys, values, &error));
            c4key_free(keys[0]);
            c4doc_free(doc);
        }
        AssertEqual(error.code, 0);
        c4enum_free(e);
        if (kLog) fprintf(stderr, ">>indexed_to:%lld ", lastSeq);
        Assert(c4indexer_end(ind, true, &error));

        C4SequenceNumber gotLastSeq = c4view_getLastSequenceIndexed(view);
        if (gotLastSeq != lastSeq)
            if (kLog) fprintf(stderr, "BUT read lastSeq=%lld! ", gotLastSeq);
        AssertEqual(gotLastSeq, lastSeq);
        AssertEqual(c4view_getLastSequenceChangedAt(view), lastSeq);
    }
    

    void queryIndexTask() {
        C4Database* database = kSharedHandle ? db : openDB();
        C4View* view = kSharedHandle ? v : openView(database);

        int i = 0;
        do {
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
            if (kLog) fprintf(stderr, "\nIndex query #%3d: ", ++i);
        } while (queryIndex(view));

        if (!kSharedHandle) {
            closeView(view);
            closeDB(database);
        }
    }

    bool queryIndex(C4View* view) {
        C4Error error;
        auto e = c4view_query(view, NULL, &error);
        Assert(e);
        if (kLog) fprintf(stderr, "{ ");

        C4SequenceNumber i = 0;
        while (c4queryenum_next(e, &error)) {
            ++i;
            //std::cerr << "Key: " << toJSON(e->key) << "\n";
            char buf[20];
            sprintf(buf, "\"doc-%05llu\"", i);
#if 1
            if (e->docSequence != i) {
                if (kLog) fprintf(stderr,"\n*** Expected %s, got %s ***\n", buf, toJSON(e->key).c_str());
                i = e->docSequence;
                continue;
            }
#else
            AssertEqual(e->docSequence, i);
#endif
            AssertEqual(toJSON(e->key), std::string(buf));
            AssertEqual(e->value, c4str("1234"));

        }
        if (kLog) fprintf(stderr, "}queried_to:%llu ", i);
        c4queryenum_free(e);
        AssertEqual(error.code, 0);
        return (i < kNumDocs);
    }
    
    CPPUNIT_TEST_SUITE( C4ThreadingTest );
    CPPUNIT_TEST( testCreateVsEnumerate );
    CPPUNIT_TEST_SUITE_END();
};

//FIX: This test is disabled until ForestDB bugs are fixed
CPPUNIT_TEST_SUITE_REGISTRATION(C4ThreadingTest);
