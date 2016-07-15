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


static const slice kDoc1ID("Doc1");
static const slice kRev1ID("1@*");

static const Revision::BodyParams kBody1 {
    slice("{\"foo\":true}"), slice("foodoc"), false, false
};
static const Revision::BodyParams kBody2 {
    slice("{\"foo\":23,\"_attachments\":{}}"), slice("foodoc"), false, true
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
        Assert(store->get(kDoc1ID) == nullptr);
        Assert(store->get(kDoc1ID, kRev1ID) == nullptr);
        AssertEqual(store->checkRevision(kDoc1ID, kRev1ID), kOlder);
    }

    
    void testInsertCASRevs() {
        // Start with CAS=17:
        Transaction t(db);
        auto rev = store->insertFromServer(kDoc1ID, 17, kBody1, t);
        Assert(rev != nullptr);
        AssertEqual(rev->docID(), kDoc1ID);
        AssertEqual(rev->body(), kBody1.body);
        AssertEqual(rev->version(), VersionVector(slice("1@$")));

        // Adding earlier CASs should do nothing:
        AssertNull(store->insertFromServer(kDoc1ID, 17, kBody1, t).get());
        AssertNull(store->insertFromServer(kDoc1ID, 10, kBody1, t).get());

        // Update to CAS=18:
        rev = store->insertFromServer(kDoc1ID, 18, kBody2, t);
        Assert(rev != nullptr);
        AssertEqual(rev->docID(), kDoc1ID);
        AssertEqual(rev->body(), kBody2.body);
        AssertEqual(rev->version(), VersionVector(slice("2@$")));

        // Previous revision (1@$) shouldn't be around:
        AssertNull(store->get(kDoc1ID, slice("1@$")).get());

        // Latest version is 18:
        rev = store->getLatestCASServerRevision(kDoc1ID);
        Assert(rev != nullptr);
        AssertEqual(rev->version(), VersionVector(slice("2@$")));
    }


    void testAddLocalRevs() {
        // Start with CAS=18:
        Transaction t(db);
        auto rev = store->insertFromServer(kDoc1ID, 18, kBody1, t);

        AssertEqual(store->getServerState(kDoc1ID),
                    (CASRevisionStore::ServerState{{slice("1@$"), 18}, {slice("1@$"), 18}}));

        // Update it locally:
        rev = store->create(kDoc1ID, rev->version(), kBody2, t);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,1@$"));

        AssertEqual(store->getServerState(kDoc1ID),
                    (CASRevisionStore::ServerState{{slice("1@$"), 18}, {slice("1@$"), 18}}));

        // Current revision is the local one:
        rev = store->get(kDoc1ID);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,1@$"));

        // Latest CAS version is 18:
        auto casrev = store->getLatestCASServerRevision(kDoc1ID);
        Assert(casrev != nullptr);
        AssertEqual(casrev->version(), VersionVector(slice("1@$")));

        // Can get revision 18 by revID:
        Assert(store->get(kDoc1ID, slice("1@$")) != nullptr);

        // Adding same CAS again should do nothing:
        AssertNull(store->insertFromServer(kDoc1ID, 17, kBody1, t).get());

        // Alright, now assume we PUT this to the server and it gets accepted as CAS 23.
        pushRev(*rev, t, 18, 23);

        AssertEqual(store->getServerState(kDoc1ID),
                    (CASRevisionStore::ServerState{{slice("1@*"), 23}, {slice("1@*"), 23}}));

        rev = store->get(kDoc1ID);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,1@$"));    // vvec hasn't changed

        // Ancestor revision 18 is gone:
        AssertNull(store->get(kDoc1ID, slice("1@$")).get());
    }


    void pushRev(Revision &rev, Transaction &t, generation expectedBaseCAS, generation newCAS) {
        generation baseCAS;
        auto baseRev = store->getBaseCASServerRevision(rev.docID(), baseCAS);
        AssertEqual(baseCAS, expectedBaseCAS);
        // ... here the rev's body & baseCAS would be sent to the server, which would return newCAS
        store->assignCAS(rev.docID(), rev.revID(), newCAS, t);
    }


    CPPUNIT_TEST_SUITE( CASRevisionStoreTest );
    CPPUNIT_TEST( testEmptyStore );
    CPPUNIT_TEST( testInsertCASRevs );
    CPPUNIT_TEST( testAddLocalRevs );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(CASRevisionStoreTest);
