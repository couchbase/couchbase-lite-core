//
//  c4ThreadingTest.cpp
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/1/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4Observer.h"
#include "c4DocEnumerator.h"
#include <assert.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std;


// The Catch library is not thread-safe, so we can't use it in this test.
#undef REQUIRE
#define REQUIRE(X) do {if (!(X)) abort();} while (0)
#undef CHECK
#define CHECK(X) do {if (!(X)) abort();} while (0)
#undef INFO
#define INFO(X)


class C4ThreadingTest : public C4Test {
public:

    static const bool kLog = false;
    static const int kNumDocs = 10000;

    static const bool kSharedHandle = false; // Use same C4Database on all threads?
    

    mutex _observerMutex;
    condition_variable _observerCond;
    bool _changesToObserve {false};


    C4ThreadingTest(int testOption)
    :C4Test(testOption)
    { }

    ~C4ThreadingTest() {
    }


    C4Database* openDB() {
        C4Database* database = c4db_open(databasePath(), c4db_getConfig(db), nullptr);
        REQUIRE(database);
        return database;
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
        C4SequenceNumber lastSequence = 0;
        do {
            {
                unique_lock<mutex> lock(_observerMutex);
                _observerCond.wait(lock, [&]{return _changesToObserve;});
                fprintf(stderr, "8");
                _changesToObserve = false;
            }

            C4DatabaseChange changes[10];
            uint32_t nDocs;
            bool external;
            while (0 < (nDocs = c4dbobs_getChanges(observer, changes, 10, &external))) {
                REQUIRE(external);
                for (auto i = 0; i < nDocs; ++i) {
                    REQUIRE(memcmp(changes[i].docID.buf, "doc-", 4) == 0);
                    lastSequence = changes[i].sequence;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (lastSequence < kNumDocs);
        c4dbobs_free(observer);
        closeDB(database);
    }
    
};


N_WAY_TEST_CASE_METHOD(C4ThreadingTest, "Threading CreateVsEnumerate", "[Threading][noisy][C]") {
    std::cerr << "\nThreading test ";

    std::thread thread1([this]{addDocsTask();});
    std::thread thread4([this]{observerTask();});

    thread1.join();
    thread4.join();
    std::cerr << "Threading test done!\n";
}
