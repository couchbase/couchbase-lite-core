//
// c4ObserverTest.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "c4Test.hh"
#include "c4Observer.h"
#include "c4.hh"


class C4ObserverTest : public C4Test {
    public:

    // This test is not dependent on different storage/versioning types.
    C4ObserverTest() :C4Test(0) { }

    ~C4ObserverTest() {
        c4docobs_free(docObserver);
        c4dbobs_free(dbObserver);
    }

    void dbObserverCalled(C4DatabaseObserver *obs) {
        CHECK(obs == dbObserver);
        ++dbCallbackCalls;
    }

    void docObserverCalled(C4DocumentObserver* obs,
                           C4Slice docID,
                           C4SequenceNumber seq)
    {
        CHECK(obs == docObserver);
        ++docCallbackCalls;
    }

    void checkChanges(std::vector<const char*> expectedDocIDs,
                      std::vector<const char*> expectedRevIDs,
                      bool expectedExternal =false) {
        C4DatabaseChange changes[100];
        bool external;
        auto changeCount = c4dbobs_getChanges(dbObserver, changes, 100, &external);
        REQUIRE(changeCount == expectedDocIDs.size());
        for (unsigned i = 0; i < changeCount; ++i) {
            CHECK(changes[i].docID == c4str(expectedDocIDs[i]));
            CHECK(changes[i].revID == c4str(expectedRevIDs[i]));
            i++;
        }
        CHECK(external == expectedExternal);
        c4dbobs_releaseChanges(changes, changeCount);
    }

    C4DatabaseObserver* dbObserver {nullptr};
    unsigned dbCallbackCalls {0};

    C4DocumentObserver* docObserver {nullptr};
    unsigned docCallbackCalls {0};
};


static void dbObserverCallback(C4DatabaseObserver* obs, void *context) {
    ((C4ObserverTest*)context)->dbObserverCalled(obs);
}

static void docObserverCallback(C4DocumentObserver* obs,
                                C4Slice docID,
                                C4SequenceNumber seq,
                                void *context)
{
    ((C4ObserverTest*)context)->docObserverCalled(obs, docID, seq);
}


TEST_CASE_METHOD(C4ObserverTest, "DB Observer", "[Observer][C]") {
    dbObserver = c4dbobs_create(db, dbObserverCallback, this);
    CHECK(dbCallbackCalls == 0);

    createRev(C4STR("A"), C4STR("1-aa"), kFleeceBody);
    CHECK(dbCallbackCalls == 1);
    createRev(C4STR("B"), C4STR("1-bb"), kFleeceBody);
    CHECK(dbCallbackCalls == 1);

    checkChanges({"A", "B"}, {"1-aa", "1-bb"});

    createRev(C4STR("B"), C4STR("2-bbbb"), kFleeceBody);
    CHECK(dbCallbackCalls == 2);
    createRev(C4STR("C"), C4STR("1-cc"), kFleeceBody);
    CHECK(dbCallbackCalls == 2);

    checkChanges({"B", "C"}, {"2-bbbb", "1-cc"});

    c4dbobs_free(dbObserver);
    dbObserver = nullptr;

    createRev(C4STR("A"), C4STR("2-aaaa"), kFleeceBody);
    CHECK(dbCallbackCalls == 2);
}


TEST_CASE_METHOD(C4ObserverTest, "Doc Observer", "[Observer][C]") {
    createRev(C4STR("A"), C4STR("1-aa"), kFleeceBody);

    docObserver = c4docobs_create(db, C4STR("A"), docObserverCallback, this);
    CHECK(docCallbackCalls == 0);

    createRev(C4STR("A"), C4STR("2-bb"), kFleeceBody);
    createRev(C4STR("B"), C4STR("1-bb"), kFleeceBody);
    CHECK(docCallbackCalls == 1);
}


TEST_CASE_METHOD(C4ObserverTest, "Multi-DB Observer", "[Observer][C]") {
    dbObserver = c4dbobs_create(db, dbObserverCallback, this);
    CHECK(dbCallbackCalls == 0);

    createRev(C4STR("A"), C4STR("1-aa"), kFleeceBody);
    CHECK(dbCallbackCalls == 1);
    createRev(C4STR("B"), C4STR("1-bb"), kFleeceBody);
    CHECK(dbCallbackCalls == 1);
    checkChanges({"A", "B"}, {"1-aa", "1-bb"});

    // Open another database on the same file:
    C4Database* otherdb = c4db_open(databasePath(), c4db_getConfig(db), nullptr);
    REQUIRE(otherdb);
    {
        TransactionHelper t(otherdb);
        createRev(otherdb, C4STR("c"), C4STR("1-cc"), kFleeceBody);
        createRev(otherdb, C4STR("d"), C4STR("1-dd"), kFleeceBody);
        createRev(otherdb, C4STR("e"), C4STR("1-ee"), kFleeceBody);
    }

    CHECK(dbCallbackCalls == 2);

    checkChanges({"c", "d", "e"}, {"1-cc", "1-dd", "1-ee"}, true);

    c4dbobs_free(dbObserver);
    dbObserver = nullptr;

    createRev(C4STR("A"), C4STR("2-aaaa"), kFleeceBody);
    CHECK(dbCallbackCalls == 2);

    c4db_close(otherdb, NULL);
    c4db_release(otherdb);
}


TEST_CASE_METHOD(C4ObserverTest, "Multi-DB Observer With Reopen", "[Observer][C]") {
    // Reproduces CBL-3013 "Continuous replicator does not push docs which are being observed"
    createRev("doc"_sl, kRevID, kFleeceBody);

    // Important step to reproduce the bug:
    reopenDB();

    // Add a doc observer:
    C4Log("---- Adding docObserver to reopened db ---");
    docObserver = c4docobs_create(db, "doc"_sl, docObserverCallback, this);
    REQUIRE(docObserver);

    // Open another database on the same file:
    C4Log("---- Opening another database instance ---");
    c4::ref<C4Database> otherdb = c4db_openAgain(db, nullptr);
    REQUIRE(otherdb);
    
    // Start a database observer on otherdb:
    dbObserver = c4dbobs_create(otherdb, dbObserverCallback, this);

    // Update the doc:
    C4Log("---- Updating doc ---");
    createRev("doc"_sl, kRev2ID, kFleeceBody);

    CHECK(docCallbackCalls == 1);
    CHECK(dbCallbackCalls == 1);        // <-- this was failing, actual value was 0
}


TEST_CASE_METHOD(C4ObserverTest, "Doc Observer Purge", "[Observer][C]") {
    createRev(C4STR("A"), C4STR("1-aa"), kFleeceBody);

    dbObserver = c4dbobs_create(db, dbObserverCallback, this);
    CHECK(dbCallbackCalls == 0);

    REQUIRE(c4db_beginTransaction(db, nullptr));
    REQUIRE(c4db_purgeDoc(db, "A"_sl, nullptr));
    REQUIRE(c4db_endTransaction(db, true, nullptr));

    CHECK(dbCallbackCalls == 1);

    checkChanges({"A"}, {""});
}


TEST_CASE_METHOD(C4ObserverTest, "Doc Observer Expiration", "[Observer][C]") {
    auto now = c4_now();

    createRev(C4STR("A"), C4STR("1-aa"), kFleeceBody);
    createRev(C4STR("B"), C4STR("1-bb"), kFleeceBody);
    REQUIRE(c4doc_setExpiration(db, "A"_sl, now - 100*1000, nullptr));
    REQUIRE(c4doc_setExpiration(db, "B"_sl, now + 100*1000, nullptr));

    dbObserver = c4dbobs_create(db, dbObserverCallback, this);
    CHECK(dbCallbackCalls == 0);

    REQUIRE(c4db_purgeExpiredDocs(db, nullptr));

    CHECK(dbCallbackCalls == 1);
    checkChanges({"A"}, {""});
}


