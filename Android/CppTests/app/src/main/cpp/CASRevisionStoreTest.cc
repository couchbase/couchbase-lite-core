//
//  CASRevisionStoreTest.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/13/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#include "CASRevisionStore.hh"

using namespace litecore;


static bool operator== (const CASRevisionStore::ServerState &a, const CASRevisionStore::ServerState &b) {
    return a.base.revID == b.base.revID && a.base.CAS == b.base.CAS
    && a.latest.revID == b.latest.revID && b.latest.CAS == b.latest.CAS;
}

std::ostream& operator<< (std::ostream &out, const slice);

//static std::ostream& operator<< (std::ostream &out, const CASRevisionStore::ServerState &state) {
//    out << "{{" << state.base.revID << ", " << state.base.CAS << "}, {"
//        << state.latest.revID << ", " << state.latest.CAS << "}}";
//    return out;
//}


#include "LiteCoreTest.hh"


static const slice kDocID("Doc1");
static const slice kRev1ID("1@*");

static const Revision::BodyParams kBody1 {
    "{\"foo\":true}"_sl, "foodoc"_sl, false, false
};
static const Revision::BodyParams kBody2 {
    "{\"foo\":23,\"_attachments\":{}}"_sl, "foodoc"_sl, false, true
};
static const Revision::BodyParams kBody3 {
    "{\"foo\":99,\"_attachments\":{}}"_sl, "foodoc"_sl, false, true
};


class CASRevisionStoreTest : public DataFileTestFixture {
public:
    
    CASRevisionStore *store {nullptr};

    CASRevisionStoreTest(int testOption)
    :DataFileTestFixture(testOption)
    {
        store = new CASRevisionStore(db);
    }

    ~CASRevisionStoreTest() {
        delete store;
    }

    void pushRev(Revision &rev, Transaction &t, generation expectedBaseCAS, generation newCAS) {
        generation baseCAS;
        auto baseRev = store->getBaseCASServerRevision(rev.docID(), baseCAS);
        REQUIRE(baseCAS == expectedBaseCAS);
        // ... here the rev's body & baseCAS would be sent to the server, which would return newCAS
        store->savedToCASServer(rev.docID(), rev.revID(), newCAS, t);
    }
};


N_WAY_TEST_CASE_METHOD(CASRevisionStoreTest, "EmptyCASStore", "[RevisionStore]") {
    REQUIRE(store->get(kDocID) == nullptr);
    REQUIRE(store->get(kDocID, kRev1ID) == nullptr);
    REQUIRE(store->checkRevision(kDocID, kRev1ID) == kOlder);
}


N_WAY_TEST_CASE_METHOD(CASRevisionStoreTest, "CAS InsertRevs", "[RevisionStore]") {
    // Start with CAS=17:
    Transaction t(db);
    auto rev = store->insertFromServer(kDocID, 17, kBody1, t);
    REQUIRE(rev != nullptr);
    REQUIRE(rev->docID() == kDocID);
    REQUIRE(rev->body() == kBody1.body);
    REQUIRE(rev->version() == VersionVector("1@$"_sl));

    // Adding earlier CASs should do nothing:
    CHECK(store->insertFromServer(kDocID, 17, kBody1, t).get() == nullptr);
    CHECK(store->insertFromServer(kDocID, 10, kBody1, t).get() == nullptr);

    // Update to CAS=18:
    rev = store->insertFromServer(kDocID, 18, kBody2, t);
    REQUIRE(rev != nullptr);
    REQUIRE(rev->docID() == kDocID);
    REQUIRE(rev->body() == kBody2.body);
    REQUIRE(rev->version() == VersionVector("2@$"_sl));

    // Previous revision (1@$) shouldn't be around:
    CHECK(store->get(kDocID, "1@$"_sl).get() == nullptr);

    // Latest version is 18:
    generation cas;
    rev = store->getLatestCASServerRevision(kDocID, cas);
    REQUIRE(rev != nullptr);
    REQUIRE(rev->version() == VersionVector("2@$"_sl));
    REQUIRE(cas == (generation)18);
    t.commit();
}


N_WAY_TEST_CASE_METHOD(CASRevisionStoreTest, "CAS AddLocalRevs", "[RevisionStore]") {
    // Start with CAS=18:
    Transaction t(db);
    auto rev = store->insertFromServer(kDocID, 18, kBody1, t);

    REQUIRE(store->getServerState(kDocID) == (CASRevisionStore::ServerState{{"1@$"_sl, 18}, {"1@$"_sl, 18}}));

    // Update it locally:
    rev = store->create(kDocID, rev->version(), kBody2, t);
    REQUIRE(rev);
    REQUIRE(rev->version().asString() == std::string("1@*,1@$"));

    REQUIRE(store->getServerState(kDocID) == (CASRevisionStore::ServerState{{"1@$"_sl, 18}, {"1@$"_sl, 18}}));

    // Current revision is the local one:
    rev = store->get(kDocID);
    REQUIRE(rev);
    REQUIRE(rev->version().asString() == std::string("1@*,1@$"));

    // Latest CAS version is 18:
    generation cas;
    auto casrev = store->getLatestCASServerRevision(kDocID, cas);
    REQUIRE(casrev != nullptr);
    REQUIRE(casrev->version() == VersionVector("1@$"_sl));
    REQUIRE(cas == (generation)18);

    // Can get revision 18 by revID:
    REQUIRE(store->get(kDocID, "1@$"_sl) != nullptr);

    // Adding same CAS again should do nothing:
    CHECK(store->insertFromServer(kDocID, 17, kBody1, t).get() == nullptr);

    // Alright, now assume we PUT this to the server and it gets accepted as CAS 23.
    pushRev(*rev, t, 18, 23);
    REQUIRE(store->getServerState(kDocID) == (CASRevisionStore::ServerState{{"1@*"_sl, 23}, {"1@*"_sl, 23}}));

    rev = store->get(kDocID);
    REQUIRE(rev);
    REQUIRE(rev->version().asString() == std::string("1@*,1@$"));    // vvec hasn't changed

    // Ancestor revision 18 is gone:
    CHECK(store->get(kDocID, "1@$"_sl).get() == nullptr);
    t.commit();
}


N_WAY_TEST_CASE_METHOD(CASRevisionStoreTest, "CAS Conflict", "[RevisionStore]") {
    // Start with CAS=18:
    Transaction t(db);
    auto rev = store->insertFromServer(kDocID, 18, kBody1, t);
    // Update it locally:
    rev = store->create(kDocID, rev->version(), kBody2, t);

    // Now pull a conflicting server revision:
    auto serverRev = store->insertFromServer(kDocID, 77, kBody2, t);
    REQUIRE(serverRev);

    REQUIRE(store->getServerState(kDocID) == (CASRevisionStore::ServerState{{"1@$"_sl, 18}, {"2@$"_sl, 77}}));

    auto currentRev = store->get(kDocID);
    REQUIRE(currentRev->revID() == alloc_slice("1@*"));
    REQUIRE(currentRev->isConflicted());

    generation cas;
    auto conflictRev = store->getLatestCASServerRevision(kDocID, cas);
    REQUIRE(conflictRev != nullptr);
    REQUIRE(conflictRev->revID() == alloc_slice("2@$"));
    REQUIRE(cas == (generation)77);
    auto baseRev = store->getBaseCASServerRevision(kDocID, cas);
    REQUIRE(baseRev != nullptr);
    REQUIRE(baseRev->revID() == alloc_slice("1@$"));
    REQUIRE(cas == (generation)18);

    // Resolve it:
    auto conflicts = std::vector<Revision*>{currentRev.get(), baseRev.get(), conflictRev.get()};
    auto resolved = store->resolveConflict(conflicts, kBody3, t);

    REQUIRE(resolved != nullptr);
    // Note: Any change to the resolved revision's body, or to the digest algorithm,
    // will cause this assertion to fail:
    REQUIRE(resolved->version().asString() == std::string("^+IAy11SY941zjp4RhcnpjFzT19k=,1@*,2@$"));
    REQUIRE_FALSE(resolved->isConflicted());

    REQUIRE(store->getServerState(kDocID) == (CASRevisionStore::ServerState{{"2@$"_sl, 77}, {"2@$"_sl, 77}}));

    CHECK(store->get(kDocID, "1@$"_sl).get() == nullptr); // old base rev is gone

    // Push the resolved version:
    pushRev(*resolved, t, 77, 99);
    t.commit();
}
