//
//  c4ThreadingTest.cpp
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/1/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4View.h"
#include "c4Observer.h"
#include "c4DocEnumerator.h"
#include <assert.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std;


#undef REQUIRE
#define REQUIRE assert
#undef CHECK
#define CHECK assert


static const char *kViewIndexPath = kTestDir "forest_temp.view.index";


class C4ThreadingTest : public C4Test {
public:

    static const bool kLog = false;
    static const int kNumDocs = 10000;

    static const bool kSharedHandle = false; // Use same C4Database on all threads?
    

    C4View* v {nullptr};
    mutex _observerMutex;
    condition_variable _observerCond;
    bool _changesToObserve {false};


    C4ThreadingTest(int testOption)
    :C4Test(testOption)
    {
        v = openView(db);
    }

    ~C4ThreadingTest() {
        closeView(v);
    }


    C4Database* openDB() {
        C4Database* database = c4db_open(databasePath(), c4db_getConfig(db), nullptr);
        REQUIRE(database);
        return database;
    }

    C4View* openView(C4Database* onDB) {
        C4View* view = c4view_open(onDB, kC4SliceNull,
                                   c4str("myview"), c4str("1"),
                                   c4db_getConfig(onDB), nullptr);
        REQUIRE(view);
        return view;
    }

    void closeView(C4View* view) {
        c4view_close(view, nullptr);
        c4view_free(view);
    }

    void closeDB(C4Database* database) {
        c4db_close(database, nullptr);
        c4db_free(database);
    }


#pragma mark - TASKS:


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

        if (kLog) Log("Enumerating documents...\n");
        int n;
        int i = 0;
        do {
            if (kLog) fprintf(stderr, "\nEnumeration #%3d: ", ++i);

            (void)c4db_getLastSequence(database);

            C4Error error;
            C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
            options.flags |= kC4IncludeBodies;
            auto e = c4db_enumerateChanges(database, 0, &options, &error);
            REQUIRE(e);

            n = 0;
            while (c4enum_next(e, &error)) {
                auto doc = c4enum_getDocument(e, &error);
                REQUIRE(doc);
                c4doc_free(doc);
                n++;
            }
            REQUIRE(error.code == 0);
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
        C4SequenceNumber oldLastSeqIndexed = c4view_getLastSequenceIndexed(view);
        C4Error error;
        C4Indexer* ind = c4indexer_begin(updateDB, &view, 1, &error);
        REQUIRE(ind);

        C4DocEnumerator* e = c4indexer_enumerateDocuments(ind, &error);
        if (!e) {
            c4indexer_end(ind, false, nullptr);
            REQUIRE(error.code == 0);
            return;
        }

        if (kLog) fprintf(stderr, "<< ");

        C4Document *doc;
        C4SequenceNumber lastSeq = oldLastSeqIndexed;
        while (nullptr != (doc = c4enum_nextDocument(e, &error))) {
            // Index 'doc':
            if (kLog) fprintf(stderr, "(#%lld) ", doc->sequence);
            if (lastSeq)
                REQUIRE(doc->sequence == lastSeq+1);
            lastSeq = doc->sequence;
            C4Key *keys[1];
            C4Slice values[1];
            keys[0] = c4key_new();
            c4key_addString(keys[0], doc->docID);
            values[0] = c4str("1234");
            REQUIRE(c4indexer_emit(ind, doc, 0, 1/*2*/, keys, values, &error));
            c4key_free(keys[0]);
            c4doc_free(doc);
        }
        REQUIRE(error.code == 0);
        c4enum_free(e);
        if (kLog) fprintf(stderr, ">>indexed_to:%lld ", lastSeq);
        REQUIRE(c4indexer_end(ind, true, &error));

        C4SequenceNumber newLastSeqIndexed = c4view_getLastSequenceIndexed(view);
        if (newLastSeqIndexed != lastSeq)
            if (kLog) fprintf(stderr, "BUT view.lastSequenceIndexed=%lld! (Started as %lld) ", newLastSeqIndexed, oldLastSeqIndexed);
        REQUIRE(newLastSeqIndexed == lastSeq);
        REQUIRE(c4view_getLastSequenceChangedAt(view) == lastSeq);
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
        auto e = c4view_query(view, nullptr, &error);
        REQUIRE(e);
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
            REQUIRE(e->docSequence == i);
#endif
            REQUIRE(toJSON(e->key) == std::string(buf));
            REQUIRE(e->value == c4str("1234"));

        }
        if (kLog) fprintf(stderr, "}queried_to:%llu ", i);
        c4queryenum_free(e);
        REQUIRE(error.code == 0);
        return (i < kNumDocs);
    }


    static void obsCallback(C4DatabaseObserver* observer, void *context) {
        ((C4ThreadingTest*)context)->observe(observer);
    }

    void observe(C4DatabaseObserver* observer) {
        fprintf(stderr, "!");
        {
            std::lock_guard<std::mutex> lock(_observerMutex);
            _changesToObserve = true;
        }
        _observerCond.notify_one();
    }


    void observerTask() {
        C4Database* database = openDB();
        auto observer = c4dbobs_create(database, obsCallback, this);
        C4SequenceNumber lastSequence;
        do {
            {
                unique_lock<mutex> lock(_observerMutex);
                _observerCond.wait(lock, [&]{return _changesToObserve;});
                fprintf(stderr, "8");
                _changesToObserve = false;
            }

            C4Slice docIDs[10];
            uint32_t nDocs;
            bool external;
            while (0 < (nDocs = c4dbobs_getChanges(observer, docIDs, 10, &lastSequence, &external))) {
                REQUIRE(external);
                for (auto i = 0; i < nDocs; ++i)
                    REQUIRE(memcmp(docIDs[i].buf, "doc-", 4) == 0);
            }
            REQUIRE(lastSequence > 0);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (lastSequence < kNumDocs);
        c4dbobs_free(observer);
        closeDB(database);
    }
    
};


N_WAY_TEST_CASE_METHOD(C4ThreadingTest, "Threading CreateVsEnumerate", "[Threading][noisy][C]") {
    std::cerr << "\nThreading test ";

    std::thread thread1([this]{addDocsTask();});
    std::thread thread2([this]{updateIndexTask();});
    std::thread thread3([this]{queryIndexTask();});
    std::thread thread4([this]{observerTask();});

    thread1.join();
    thread2.join();
    thread3.join();
    thread4.join();
    std::cerr << "Threading test done!\n";
}
