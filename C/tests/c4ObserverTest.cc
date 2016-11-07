//
//  c4ObserverTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 11/7/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4Observer.h"

using namespace fleece;


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

    void checkChanges(std::vector<const char*> expectedDocIDs) {
        C4Slice docIDs[100];
        C4SequenceNumber lastSeq;
        auto changeCount = c4dbobs_getChanges(dbObserver, docIDs, 100, &lastSeq);
        REQUIRE(changeCount == expectedDocIDs.size());
        unsigned i = 0;
        for (auto docID : expectedDocIDs) {
            CHECK(docIDs[i++] == c4str(docID));
        }
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

    createRev(C4STR("A"), C4STR("1-aa"), kBody);
    CHECK(dbCallbackCalls == 1);
    createRev(C4STR("B"), C4STR("1-bb"), kBody);
    CHECK(dbCallbackCalls == 1);

    checkChanges({"A", "B"});

    createRev(C4STR("B"), C4STR("2-bbbb"), kBody);
    CHECK(dbCallbackCalls == 2);
    createRev(C4STR("C"), C4STR("1-cc"), kBody);
    CHECK(dbCallbackCalls == 2);

    checkChanges({"B", "C"});

    c4dbobs_free(dbObserver);
    dbObserver = nullptr;

    createRev(C4STR("A"), C4STR("2-aaaa"), kBody);
    CHECK(dbCallbackCalls == 2);
}



TEST_CASE_METHOD(C4ObserverTest, "Doc Observer", "[Observer][C]") {
    createRev(C4STR("A"), C4STR("1-aa"), kBody);

    docObserver = c4docobs_create(db, C4STR("A"), docObserverCallback, this);
    CHECK(docCallbackCalls == 0);

    createRev(C4STR("A"), C4STR("2-bb"), kBody);
    createRev(C4STR("B"), C4STR("1-bb"), kBody);
    CHECK(docCallbackCalls == 1);
}
