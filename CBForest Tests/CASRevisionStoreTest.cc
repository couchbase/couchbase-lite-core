//
//  CASRevisionStoreTest.cc
//  CBForest
//
//  Created by Jens Alfke on 7/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "CASRevisionStore.hh"

using namespace cbforest;


static bool operator== (const CASRevisionStore::ServerState &a, const CASRevisionStore::ServerState &b) {
    return a.base.revID == b.base.revID && a.base.CAS == b.base.CAS
    && a.latest.revID == b.latest.revID && b.latest.CAS == b.latest.CAS;
}

std::ostream& operator<< (std::ostream &out, const slice);

static std::ostream& operator<< (std::ostream &out, const CASRevisionStore::ServerState &state) {
    out << "{{" << state.base.revID << ", " << state.base.CAS << "}, {"
        << state.latest.revID << ", " << state.latest.CAS << "}}";
    return out;
}


#include "CBForestTest.hh"


static const slice kDocID("Doc1");
static const slice kRev1ID("1@*");

static const Revision::BodyParams kBody1 {
    slice("{\"foo\":true}"), slice("foodoc"), false, false
};
static const Revision::BodyParams kBody2 {
    slice("{\"foo\":23,\"_attachments\":{}}"), slice("foodoc"), false, true
};
static const Revision::BodyParams kBody3 {
    slice("{\"foo\":99,\"_attachments\":{}}"), slice("foodoc"), false, true
};


class CASRevisionStoreTest : public DatabaseTestFixture {
public:
    
    CASRevisionStore *store {NULL};

    void setUp() {
        DatabaseTestFixture::setUp();
        store = new CASRevisionStore(db);
    }

    void tearDown() {
        delete store;
        DatabaseTestFixture::tearDown();
    }


    void testEmptyStore() {
        Assert(store->get(kDocID) == nullptr);
        Assert(store->get(kDocID, kRev1ID) == nullptr);
        AssertEqual(store->checkRevision(kDocID, kRev1ID), kOlder);
    }

    
    void testInsertCASRevs() {
        // Start with CAS=17:
        Transaction t(db);
        auto rev = store->insertFromServer(kDocID, 17, kBody1, t);
        Assert(rev != nullptr);
        AssertEqual(rev->docID(), kDocID);
        AssertEqual(rev->body(), kBody1.body);
        AssertEqual(rev->version(), VersionVector(slice("1@$")));

        // Adding earlier CASs should do nothing:
        AssertNull(store->insertFromServer(kDocID, 17, kBody1, t).get());
        AssertNull(store->insertFromServer(kDocID, 10, kBody1, t).get());

        // Update to CAS=18:
        rev = store->insertFromServer(kDocID, 18, kBody2, t);
        Assert(rev != nullptr);
        AssertEqual(rev->docID(), kDocID);
        AssertEqual(rev->body(), kBody2.body);
        AssertEqual(rev->version(), VersionVector(slice("2@$")));

        // Previous revision (1@$) shouldn't be around:
        AssertNull(store->get(kDocID, slice("1@$")).get());

        // Latest version is 18:
        generation cas;
        rev = store->getLatestCASServerRevision(kDocID, cas);
        Assert(rev != nullptr);
        AssertEqual(rev->version(), VersionVector(slice("2@$")));
        AssertEqual(cas, (generation)18);
    }


    void testAddLocalRevs() {
        // Start with CAS=18:
        Transaction t(db);
        auto rev = store->insertFromServer(kDocID, 18, kBody1, t);

        AssertEqual(store->getServerState(kDocID),
                    (CASRevisionStore::ServerState{{slice("1@$"), 18}, {slice("1@$"), 18}}));

        // Update it locally:
        rev = store->create(kDocID, rev->version(), kBody2, t);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,1@$"));

        AssertEqual(store->getServerState(kDocID),
                    (CASRevisionStore::ServerState{{slice("1@$"), 18}, {slice("1@$"), 18}}));

        // Current revision is the local one:
        rev = store->get(kDocID);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,1@$"));

        // Latest CAS version is 18:
        generation cas;
        auto casrev = store->getLatestCASServerRevision(kDocID, cas);
        Assert(casrev != nullptr);
        AssertEqual(casrev->version(), VersionVector(slice("1@$")));
        AssertEqual(cas, (generation)18);

        // Can get revision 18 by revID:
        Assert(store->get(kDocID, slice("1@$")) != nullptr);

        // Adding same CAS again should do nothing:
        AssertNull(store->insertFromServer(kDocID, 17, kBody1, t).get());

        // Alright, now assume we PUT this to the server and it gets accepted as CAS 23.
        pushRev(*rev, t, 18, 23);
        AssertEqual(store->getServerState(kDocID),
                    (CASRevisionStore::ServerState{{slice("1@*"), 23}, {slice("1@*"), 23}}));

        rev = store->get(kDocID);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,1@$"));    // vvec hasn't changed

        // Ancestor revision 18 is gone:
        AssertNull(store->get(kDocID, slice("1@$")).get());
    }


    void testConflict() {
        // Start with CAS=18:
        Transaction t(db);
        auto rev = store->insertFromServer(kDocID, 18, kBody1, t);
        // Update it locally:
        rev = store->create(kDocID, rev->version(), kBody2, t);

        // Now pull a conflicting server revision:
        auto serverRev = store->insertFromServer(kDocID, 77, kBody2, t);
        Assert(serverRev);

        AssertEqual(store->getServerState(kDocID),
                    (CASRevisionStore::ServerState{{slice("1@$"), 18}, {slice("2@$"), 77}}));

        auto currentRev = store->get(kDocID);
        AssertEqual(currentRev->revID(), alloc_slice("1@*"));
        Assert(currentRev->isConflicted());

        generation cas;
        auto conflictRev = store->getLatestCASServerRevision(kDocID, cas);
        Assert(conflictRev != nullptr);
        AssertEqual(conflictRev->revID(), alloc_slice("2@$"));
        AssertEqual(cas, (generation)77);
        auto baseRev = store->getBaseCASServerRevision(kDocID, cas);
        Assert(baseRev != nullptr);
        AssertEqual(baseRev->revID(), alloc_slice("1@$"));
        AssertEqual(cas, (generation)18);

        // Resolve it:
        auto conflicts = std::vector<Revision*>{currentRev.get(), baseRev.get(), conflictRev.get()};
        auto resolved = store->resolveConflict(conflicts, kBody3, t);

        Assert(resolved != nullptr);
        // Note: Any change to the resolved revision's body, or to the digest algorithm,
        // will cause this assertion to fail:
        AssertEqual(resolved->version().asString(), std::string("^+IAy11SY941zjp4RhcnpjFzT19k=,1@*,2@$"));
        Assert(!resolved->isConflicted());

        AssertEqual(store->getServerState(kDocID),
                    (CASRevisionStore::ServerState{{slice("2@$"), 77}, {slice("2@$"), 77}}));

        AssertNull(store->get(kDocID, slice("1@$")).get()); // old base rev is gone

        // Push the resolved version:
        pushRev(*resolved, t, 77, 99);
    }


    void pushRev(Revision &rev, Transaction &t, generation expectedBaseCAS, generation newCAS) {
        generation baseCAS;
        auto baseRev = store->getBaseCASServerRevision(rev.docID(), baseCAS);
        AssertEqual(baseCAS, expectedBaseCAS);
        // ... here the rev's body & baseCAS would be sent to the server, which would return newCAS
        store->savedToCASServer(rev.docID(), rev.revID(), newCAS, t);
    }


    CPPUNIT_TEST_SUITE( CASRevisionStoreTest );
    CPPUNIT_TEST( testEmptyStore );
    CPPUNIT_TEST( testInsertCASRevs );
    CPPUNIT_TEST( testAddLocalRevs );
    CPPUNIT_TEST( testConflict );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(CASRevisionStoreTest);
