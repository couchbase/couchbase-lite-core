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

#include "c4Test.hh"  // IWYU pragma: keep
#include "c4Observer.h"
#include "c4Collection.h"

class C4ObserverTest : public C4Test {
  public:
    slice kDocARev1, kDocBRev1, kDocCRev1, kDocDRev1, kDocERev1, kDocARev2, kDocBRev2, kDocBRev2History;

    explicit C4ObserverTest(int which) : C4Test(which) {
        if ( isRevTrees() ) {
            kDocARev1        = "1-aa";
            kDocBRev1        = "1-bb";
            kDocCRev1        = "1-cc";
            kDocDRev1        = "1-dd";
            kDocERev1        = "1-ee";
            kDocARev2        = "2-aaaa";
            kDocBRev2        = "2-bbbb";
            kDocBRev2History = kDocBRev2;
        } else {
            kDocARev1        = "1@AliceAliceAliceAliceAA";
            kDocBRev1        = "1@BobBobBobBobBobBobBobA";
            kDocCRev1        = "1@CarolCarolCarolCarolCA";
            kDocDRev1        = "1@DaveDaveDaveDaveDaveDA";
            kDocERev1        = "1@EnidEnidEnidEnidEnidEA";
            kDocARev2        = "1@BobBobBobBobBobBobBobA";
            kDocBRev2        = "1@NorbertNorbertNorbertA";
            kDocBRev2History = "1@NorbertNorbertNorbertA,1@BobBobBobBobBobBobBobA";
        }
    }
#if SkipVersionVectorTest
    static const int numberOfOptions = 1;
#else
    static const int numberOfOptions = 2;  // rev-tree, vector; no need to test encryption
#endif

    ~C4ObserverTest() {
        c4docobs_free(docObserver);
        c4dbobs_free(dbObserver);
    }

    void dbObserverCalled(C4DatabaseObserver* obs) {
        CHECK(obs == dbObserver);
        ++dbCallbackCalls;
    }

    void docObserverCalled(C4DocumentObserver* obs, C4Collection* collection, C4Slice docID, C4SequenceNumber seq) {
        CHECK(obs == docObserver);
        ++docCallbackCalls;
        lastDocCallbackDocID    = docID;
        lastDocCallbackSequence = seq;
        lastCallbackCollection  = collection;
    }

    void checkChanges(C4Collection* expectedCollection, std::vector<slice> expectedDocIDs,
                      std::vector<slice> expectedRevIDs, bool expectedExternal = false) const {
        C4DatabaseChange changes[100];
        auto             observation = c4dbobs_getChanges(dbObserver, changes, 100);
        REQUIRE(observation.numChanges == expectedDocIDs.size());
        CHECK(observation.collection == expectedCollection);
        for ( unsigned i = 0; i < observation.numChanges; ++i ) {
            CHECK(changes[i].docID == expectedDocIDs[i]);
            CHECK(changes[i].revID == expectedRevIDs[i]);
        }
        CHECK(observation.external == expectedExternal);
        c4dbobs_releaseChanges(changes, observation.numChanges);
    }

    C4DatabaseObserver* dbObserver{nullptr};
    C4Collection*       openCollection{nullptr};
    unsigned            dbCallbackCalls{0};

    C4DocumentObserver* docObserver{nullptr};
    unsigned            docCallbackCalls{0};
    alloc_slice         lastDocCallbackDocID;
    C4SequenceNumber    lastDocCallbackSequence = 0;
    C4Collection*       lastCallbackCollection{nullptr};
};

static void dbObserverCallback(C4DatabaseObserver* obs, void* context) {
    ((C4ObserverTest*)context)->dbObserverCalled(obs);
}

static void docObserverCallback(C4DocumentObserver* obs, C4Collection* collection, C4Slice docID, C4SequenceNumber seq,
                                void* context) {
    ((C4ObserverTest*)context)->docObserverCalled(obs, collection, docID, seq);
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "DB Observer", "[Observer][C]") {
    auto spec      = C4CollectionSpec{"customScope"_sl, "customCollection"_sl};
    openCollection = c4db_createCollection(db, spec, ERROR_INFO());
    REQUIRE(openCollection);

    SECTION("Default Collection") {
        C4Collection* defaultColl = requireCollection(db);
        dbObserver                = c4dbobs_createOnCollection(defaultColl, dbObserverCallback, this, ERROR_INFO());
        REQUIRE(dbObserver);
        auto* expectedCollection = requireCollection(db);
        CHECK(dbCallbackCalls == 0);

        createRev("A"_sl, kDocARev1, kFleeceBody);
        CHECK(dbCallbackCalls == 1);
        createRev("B"_sl, kDocBRev1, kFleeceBody);
        CHECK(dbCallbackCalls == 1);

        checkChanges(expectedCollection, {"A", "B"}, {kDocARev1, kDocBRev1});

        createRev("B"_sl, kDocBRev2, kFleeceBody);
        CHECK(dbCallbackCalls == 2);
        createRev("C"_sl, kDocCRev1, kFleeceBody);
        CHECK(dbCallbackCalls == 2);

        checkChanges(expectedCollection, {"B", "C"}, {kDocBRev2History, kDocCRev1});

        // Other collections don't trigger callback
        createRev(openCollection, "A"_sl, kDocARev1, kFleeceBody);
        CHECK(dbCallbackCalls == 2);

        c4dbobs_free(dbObserver);
        dbObserver = nullptr;

        createRev("A"_sl, kDocARev2, kFleeceBody);
        CHECK(dbCallbackCalls == 2);
    }

    SECTION("Custom Collection") {
        dbObserver = c4dbobs_createOnCollection(openCollection, dbObserverCallback, this, ERROR_INFO());
        REQUIRE(dbObserver);
        CHECK(dbCallbackCalls == 0);

        createRev(openCollection, "A"_sl, kDocARev1, kFleeceBody);
        CHECK(dbCallbackCalls == 1);
        createRev(openCollection, "B"_sl, kDocBRev1, kFleeceBody);
        CHECK(dbCallbackCalls == 1);

        checkChanges(openCollection, {"A", "B"}, {kDocARev1, kDocBRev1});

        createRev(openCollection, "B"_sl, kDocBRev2, kFleeceBody);
        CHECK(dbCallbackCalls == 2);
        createRev(openCollection, "C"_sl, kDocCRev1, kFleeceBody);
        CHECK(dbCallbackCalls == 2);

        checkChanges(openCollection, {"B", "C"}, {kDocBRev2History, kDocCRev1});

        // Other collections don't trigger callback
        createRev("A"_sl, kDocARev1, kFleeceBody);
        CHECK(dbCallbackCalls == 2);

        c4dbobs_free(dbObserver);
        dbObserver = nullptr;

        createRev(openCollection, "A"_sl, kDocARev2, kFleeceBody);
        CHECK(dbCallbackCalls == 2);
    }
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Doc Observer", "[Observer][C]") {
    SECTION("Default Collection") {
        C4Collection* defaultColl = requireCollection(db);
        createRev("A"_sl, kDocARev1, kFleeceBody);

        docObserver = c4docobs_createWithCollection(defaultColl, "A"_sl, docObserverCallback, this, ERROR_INFO());
        REQUIRE(docObserver);
        CHECK(docCallbackCalls == 0);

        createRev("A"_sl, kDocARev2, kFleeceBody);
        createRev("B"_sl, kDocBRev1, kFleeceBody);
        CHECK(docCallbackCalls == 1);
        CHECK(lastDocCallbackDocID == "A");
        CHECK(lastDocCallbackSequence == 2);
        CHECK(lastCallbackCollection == requireCollection(db));
    }

    SECTION("Custom Collection") {
        auto spec      = C4CollectionSpec{"customScope"_sl, "customCollection"_sl};
        openCollection = c4db_createCollection(db, spec, ERROR_INFO());
        REQUIRE(openCollection);

        createRev(openCollection, "A"_sl, kDocARev1, kFleeceBody);

        docObserver = c4docobs_createWithCollection(openCollection, "A"_sl, docObserverCallback, this, ERROR_INFO());
        REQUIRE(docObserver);
        CHECK(docCallbackCalls == 0);

        createRev(openCollection, "A"_sl, kDocARev2, kFleeceBody);
        createRev(openCollection, "B"_sl, kDocBRev1, kFleeceBody);
        CHECK(docCallbackCalls == 1);
        CHECK(lastDocCallbackDocID == "A");
        CHECK(lastDocCallbackSequence == 2);
        CHECK(lastCallbackCollection == openCollection);
    }
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Multi-DB Observer", "[Observer][C]") {
    auto* expectedColl = requireCollection(db);
    dbObserver         = c4dbobs_createOnCollection(expectedColl, dbObserverCallback, this, ERROR_INFO());
    REQUIRE(dbObserver);
    CHECK(dbCallbackCalls == 0);

    createRev("A"_sl, kDocARev1, kFleeceBody);
    CHECK(dbCallbackCalls == 1);
    createRev("B"_sl, kDocBRev1, kFleeceBody);
    CHECK(dbCallbackCalls == 1);
    checkChanges(expectedColl, {"A", "B"}, {kDocARev1, kDocBRev1});

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

    checkChanges(expectedColl, {"c", "d", "e"}, {kDocCRev1, kDocDRev1, kDocERev1}, true);

    c4dbobs_free(dbObserver);
    dbObserver = nullptr;

    createRev("A"_sl, kDocARev2, kFleeceBody);
    CHECK(dbCallbackCalls == 2);

    CHECK(c4db_close(otherdb, nullptr));
    c4db_release(otherdb);
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Multi-DB Observer With Reopen", "[Observer][C]") {
    // Reproduces CBL-3013 "Continuous replicator does not push docs which are being observed"
    createRev("doc"_sl, kRevID, kFleeceBody);

    // Important step to reproduce the bug:
    reopenDB();

    C4Collection* defaultColl = requireCollection(db);

    // Add a doc observer:
    C4Log("---- Adding docObserver to reopened db ---");
    docObserver = c4docobs_createWithCollection(defaultColl, "doc"_sl, docObserverCallback, this, ERROR_INFO());
    REQUIRE(docObserver);

    // Open another database on the same file:
    C4Log("---- Opening another database instance ---");
    c4::ref<C4Database> otherdb = c4db_openAgain(db, ERROR_INFO());
    REQUIRE(otherdb);

    C4Collection* otherDefaultColl = requireCollection(otherdb);

    // Start a database observer on otherdb:
    dbObserver = c4dbobs_createOnCollection(otherDefaultColl, dbObserverCallback, this, ERROR_INFO());
    REQUIRE(dbObserver);

    // Update the doc:
    C4Log("---- Updating doc ---");
    createRev("doc"_sl, kRev2ID, kFleeceBody);

    CHECK(docCallbackCalls == 1);
    CHECK(dbCallbackCalls == 1);  // <-- this was failing, actual value was 0
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Doc Observer Purge", "[Observer][C]") {
    createRev("A"_sl, kDocARev1, kFleeceBody);

    C4Collection* defaultColl = requireCollection(db);

    dbObserver = c4dbobs_createOnCollection(defaultColl, dbObserverCallback, this, ERROR_INFO());
    REQUIRE(dbObserver);
    CHECK(dbCallbackCalls == 0);

    REQUIRE(c4db_beginTransaction(db, ERROR_INFO()));
    REQUIRE(c4coll_purgeDoc(defaultColl, "A"_sl, ERROR_INFO()));
    REQUIRE(c4db_endTransaction(db, true, ERROR_INFO()));

    CHECK(dbCallbackCalls == 1);
    checkChanges(requireCollection(db), {"A"}, {""});
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Doc Observer Expiration", "[Observer][C]") {
    auto now = c4_now();

    createRev("A"_sl, kDocARev1, kFleeceBody);
    createRev("B"_sl, kDocBRev1, kFleeceBody);

    C4Collection* defaultColl = requireCollection(db);

    dbObserver = c4dbobs_createOnCollection(defaultColl, dbObserverCallback, this, ERROR_INFO());
    REQUIRE(dbObserver);
    CHECK(dbCallbackCalls == 0);

    REQUIRE(c4coll_setDocExpiration(defaultColl, "A"_sl, now - 100 * 1000, nullptr));
    REQUIRE(c4coll_setDocExpiration(defaultColl, "B"_sl, now + 100 * 1000, nullptr));

    auto isDocExpired = [&] {
        auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
        c4::ref<C4Document> doc         = c4coll_getDoc(defaultColl, "A"_sl, true, kDocGetAll, nullptr);
        return doc == nullptr;
    };
    REQUIRE_BEFORE(5s, isDocExpired());

    CHECK(dbCallbackCalls == 1);
    checkChanges(requireCollection(db), {"A"}, {""}, true);
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Observer Free After DB Close", "[Observer][C]") {
    C4Collection* defaultColl = requireCollection(db);

    // CBL-3193: Freeing a document observer after database close caused a SIGSEGV
    dbObserver = c4dbobs_createOnCollection(defaultColl, dbObserverCallback, this, ERROR_INFO());
    REQUIRE(dbObserver);
    docObserver = c4docobs_createWithCollection(defaultColl, C4STR("doc1"), docObserverCallback, this, ERROR_INFO());
    REQUIRE(docObserver);
    closeDB();
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Observer Free After Collection Delete", "[Observer][C][CBL-3602]") {
    C4Collection* coll = c4db_createCollection(db, {"bar"_sl, "foo"_sl}, ERROR_INFO());
    REQUIRE(coll);
    auto* dbObs  = c4dbobs_createOnCollection(coll, dbObserverCallback, this, ERROR_INFO());
    auto* docObs = c4docobs_createWithCollection(coll, C4STR("doc1"), docObserverCallback, this, ERROR_INFO());
    REQUIRE(c4db_deleteCollection(db, {"bar"_sl, "foo"_sl}, ERROR_INFO()));
    CHECK(c4db_getCollection(db, {"bar"_sl, "foo"_sl}, ERROR_INFO()) == nullptr);

    // Previously this caused a SIGSEGV, and beyond that an exception
    // because deleting the collection invalidates several naked pointers
    // and references inside these observers.  The solution was to have the
    // observers retain their collections, and check if they are valid
    // when destructing.  If not valid, perform steps to wipe the bad refs
    c4dbobs_free(dbObs);
    c4docobs_free(docObs);
}

N_WAY_TEST_CASE_METHOD(C4ObserverTest, "Create Observer On Deleted Collection", "[Observer][C][CBL-3599]") {
    auto deleted = c4::ref<C4Collection>::retaining(c4db_createCollection(db, {"wrong"_sl, "oops"_sl}, ERROR_INFO()));
    REQUIRE(deleted);

    REQUIRE(c4db_deleteCollection(db, {"wrong"_sl, "oops"_sl}, ERROR_INFO()));
    REQUIRE(!c4db_getCollection(db, {"wrong"_sl, "oops"_sl}, ERROR_INFO()));

    {
        ExpectingExceptions x;

        C4Error err{};
        CHECK(!c4dbobs_createOnCollection(deleted, dbObserverCallback, nullptr, &err));
        CHECK(err.domain == LiteCoreDomain);
        CHECK(err.code == kC4ErrorNotOpen);

        err = {};
        CHECK(!c4docobs_createWithCollection(deleted, C4STR("doc1"), docObserverCallback, nullptr, &err));
        CHECK(err.domain == LiteCoreDomain);
        CHECK(err.code == kC4ErrorNotOpen);
    }
}
