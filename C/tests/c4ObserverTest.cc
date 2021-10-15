//
// c4ObserverTest.cc
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
#include <thread>


class C4ObserverTest : public C4Test {
    public:

    slice kDocARev1, kDocBRev1, kDocCRev1, kDocDRev1, kDocERev1,
          kDocARev2, kDocBRev2, kDocBRev2History;

    C4ObserverTest(int which) :C4Test(which) {
        if (isRevTrees()) {
            kDocARev1 = "1-aa";
            kDocBRev1 = "1-bb";
            kDocCRev1 = "1-cc";
            kDocDRev1 = "1-dd";
            kDocERev1 = "1-ee";
            kDocARev2 = "2-aaaa";
            kDocBRev2 = "2-bbbb";
            kDocBRev2History = kDocBRev2;
        } else {
            kDocARev1 = "1@aa";
            kDocBRev1 = "1@bb";
            kDocCRev1 = "1@cc";
            kDocDRev1 = "1@dd";
            kDocERev1 = "1@ee";
            kDocARev2 = "1@b0b";
            kDocBRev2 = "1@f00";
            kDocBRev2History = "1@f00,1@bb";
        }
    }
#if SkipVersionVectorTest
    static const int numberOfOptions = 1;
#else
    static const int numberOfOptions = 2;       // rev-tree, vector; no need to test encryption
#endif

    ~C4ObserverTest() {
        c4docobs_free(docObserver);
        c4dbobs_free(dbObserver);
    }

    void dbObserverCalled(C4DatabaseObserver *obs) {
        CHECK(obs == dbObserver);
        ++dbCallbackCalls;
        C4Log("=== dbObserverCalled #%d", dbCallbackCalls);
    }

    void logObservedChanges(C4DatabaseObserver *obs) {
        C4CollectionChange changes[100];
        bool external;
        int n;
        do {
            n = c4dbobs_getChanges(obs, changes, 100, &external);
            for (int i = 0; i < n; i++) {
                C4Log("Seq %3llu: '%.*s' %.*s", changes[i].sequence,
                      FMTSLICE(changes[i].docID), FMTSLICE(changes[i].revID));
            }
        } while (n > 0);
    }

    void docObserverCalled(C4DocumentObserver* obs,
                           C4Slice docID,
                           C4SequenceNumber seq)
    {
        CHECK(obs == docObserver);
        ++docCallbackCalls;
        lastDocCallbackDocID = docID;
        lastDocCallbackSequence = seq;
    }

    void checkChanges(std::vector<slice> expectedDocIDs,
                      std::vector<slice> expectedRevIDs,
                      bool expectedExternal =false) {
        C4DatabaseChange changes[100];
        bool external;
        auto changeCount = c4dbobs_getChanges(dbObserver, changes, 100, &external);
        REQUIRE(changeCount == expectedDocIDs.size());
        for (unsigned i = 0; i < changeCount; ++i) {
            CHECK(changes[i].docID == expectedDocIDs[i]);
            CHECK(changes[i].revID == expectedRevIDs[i]);
            i++;
        }
        CHECK(external == expectedExternal);
        c4dbobs_releaseChanges(changes, changeCount);
    }

    C4DatabaseObserver* dbObserver {nullptr};
    unsigned dbCallbackCalls {0};

    C4DocumentObserver* docObserver {nullptr};
    unsigned docCallbackCalls {0};
    alloc_slice lastDocCallbackDocID;
    C4SequenceNumber lastDocCallbackSequence = 0;
    bool logChanges = false;
};


static void dbObserverCallback(C4DatabaseObserver* obs, void *context) {
    ((C4ObserverTest*)context)->dbObserverCalled(obs);
}

static void dbObserverLoggingCallback(C4DatabaseObserver* obs, void *context) {
    ((C4ObserverTest*)context)->dbObserverCalled(obs);
    ((C4ObserverTest*)context)->logObservedChanges(obs);
}

static void docObserverCallback(C4DocumentObserver* obs,
                                C4Slice docID,
                                C4SequenceNumber seq,
                                void *context)
{
    ((C4ObserverTest*)context)->docObserverCalled(obs, docID, seq);
}


N_WAY_TEST_CASE_METHOD(C4ObserverTest, "DB Observer", "[Observer][C]") {
    dbObserver = c4dbobs_create(db, dbObserverCallback, this);
    CHECK(dbCallbackCalls == 0);

    createRev("A"_sl, kDocARev1, kFleeceBody);
    CHECK(dbCallbackCalls == 1);
    createRev("B"_sl, kDocBRev1, kFleeceBody);
    CHECK(dbCallbackCalls == 1);

    checkChanges({"A", "B"}, {kDocARev1, kDocBRev1});

    createRev("B"_sl, kDocBRev2, kFleeceBody);
    CHECK(dbCallbackCalls == 2);
    createRev("C"_sl, kDocCRev1, kFleeceBody);
    CHECK(dbCallbackCalls == 2);

    checkChanges({"B", "C"}, {kDocBRev2History, kDocCRev1});

    c4dbobs_free(dbObserver);
    dbObserver = nullptr;

    createRev("A"_sl, kDocARev2, kFleeceBody);
    CHECK(dbCallbackCalls == 2);
}


N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Doc Observer", "[Observer][C]") {
    createRev("A"_sl, kDocARev1, kFleeceBody);

    docObserver = c4docobs_create(db, "A"_sl, docObserverCallback, this);
    CHECK(docCallbackCalls == 0);

    createRev("A"_sl, kDocARev2, kFleeceBody);
    createRev("B"_sl, kDocBRev1, kFleeceBody);
    CHECK(docCallbackCalls == 1);
    CHECK(lastDocCallbackDocID == "A");
    CHECK(lastDocCallbackSequence == 2);
}


N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Multi-DB Observer", "[Observer][C]") {
    dbObserver = c4dbobs_create(db, dbObserverCallback, this);
    CHECK(dbCallbackCalls == 0);

    createRev("A"_sl, kDocARev1, kFleeceBody);
    CHECK(dbCallbackCalls == 1);
    createRev("B"_sl, kDocBRev1, kFleeceBody);
    CHECK(dbCallbackCalls == 1);
    checkChanges({"A", "B"}, {kDocARev1, kDocBRev1});

    // Open another database on the same file:
    C4Database* otherdb = c4db_openAgain(db, nullptr);
    REQUIRE(otherdb);
    {
        TransactionHelper t(otherdb);
        createRev(otherdb, "c"_sl, kDocCRev1, kFleeceBody);
        createRev(otherdb, "d"_sl, kDocDRev1, kFleeceBody);
        createRev(otherdb, "e"_sl, kDocERev1, kFleeceBody);
    }

    CHECK(dbCallbackCalls == 2);

    checkChanges({"c", "d", "e"}, {kDocCRev1, kDocDRev1, kDocERev1}, true);

    c4dbobs_free(dbObserver);
    dbObserver = nullptr;

    createRev("A"_sl, kDocARev2, kFleeceBody);
    CHECK(dbCallbackCalls == 2);

    c4db_close(otherdb, NULL);
    c4db_release(otherdb);
}


N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Doc Observer Purge", "[Observer][C]") {
    createRev("A"_sl, kDocARev1, kFleeceBody);

    dbObserver = c4dbobs_create(db, dbObserverCallback, this);
    CHECK(dbCallbackCalls == 0);

    REQUIRE(c4db_beginTransaction(db, nullptr));
    REQUIRE(c4db_purgeDoc(db, "A"_sl, nullptr));
    REQUIRE(c4db_endTransaction(db, true, nullptr));

    CHECK(dbCallbackCalls == 1);

    checkChanges({"A"}, {""});
}


N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Doc Observer Expiration", "[Observer][C]") {
    auto now = c4_now();

    createRev("A"_sl, kDocARev1, kFleeceBody);
    createRev("B"_sl, kDocBRev1, kFleeceBody);

    dbObserver = c4dbobs_create(db, dbObserverCallback, this);
    CHECK(dbCallbackCalls == 0);

    REQUIRE(c4doc_setExpiration(db, "A"_sl, now - 100*1000, nullptr));
    REQUIRE(c4doc_setExpiration(db, "B"_sl, now + 100*1000, nullptr));

    auto isDocExpired = [&]{
        c4::ref<C4Document> doc = c4db_getDoc(db, "A"_sl, true, kDocGetAll, nullptr);
        return doc == nullptr;
    };
    REQUIRE_BEFORE(5s, isDocExpired());

    CHECK(dbCallbackCalls == 1);
    checkChanges({"A"}, {""}, true);
}
