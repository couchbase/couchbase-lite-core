//
// c4ThreadingTest.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"
#include "c4Observer.h"
#include "c4DocEnumerator.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std;


// The Catch library is not thread-safe, so we can't use it in this test.
#undef REQUIRE
#define REQUIRE(X) C4Assert(X)
#undef CHECK
#define CHECK(X) C4Assert(X)
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
    C4LogLevel _oldDbLogLevel;


    C4ThreadingTest(int testOption)
    :C4Test(testOption)
    {
        // Suppress the zillions of "begin transaction", "commit transaction" logs from this test: 
        auto dbDomain = c4log_getDomain("DB", false);
        _oldDbLogLevel = c4log_getLevel(dbDomain);
        c4log_setLevel(dbDomain, kC4LogWarning);
    }

    ~C4ThreadingTest() {
        auto dbDomain = c4log_getDomain("DB", false);
        c4log_setLevel(dbDomain, _oldDbLogLevel);
    }


    C4Database* openDB() {
        C4Database* database = c4db_openNamed(kDatabaseName, &dbConfig(), nullptr);
        REQUIRE(database);
        return database;
    }

    void closeDB(C4Database* database) {
        c4db_close(database, nullptr);
        c4db_release(database);
    }


#pragma mark - TASKS:


    void addDocsTask() {
        // This implicitly uses the 'db' connection created (but not used) by the main thread
        if (kLog) fprintf(stderr, "Adding documents...\n");
        for (int i = 1; i <= kNumDocs; i++) {
            if (kLog) fprintf(stderr, "(%d) ", i); else if (i%10 == 0) fprintf(stderr, ":");
            char docID[20];
            sprintf(docID, "doc-%05d", i);
            createRev(c4str(docID), kRevID, kFleeceBody);
            //std::this_thread::sleep_for(100us);
        }
    }


#if 0 // unused
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
            auto e = c4db_enumerateChanges(database, 0, &options, ERROR_INFO(error));
            REQUIRE(e);

            n = 0;
            while (c4enum_next(e, ERROR_INFO(error))) {
                auto doc = c4enum_getDocument(e, ERROR_INFO(error));
                REQUIRE(doc);
                c4doc_release(doc);
                n++;
            }
            REQUIRE(error.code == 0);
            c4enum_free(e);
            if (kLog) fprintf(stderr, "-- %d docs", n);
        } while (n < kNumDocs);
    }
#endif


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
                c4dbobs_releaseChanges(changes, nDocs);
            }

            std::this_thread::sleep_for(100ms);
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
