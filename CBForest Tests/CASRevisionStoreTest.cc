//
//  CASRevisionStoreTest.cc
//  CBForest
//
//  Created by Jens Alfke on 7/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "CASRevisionStore.hh"
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
        auto rev = store->insertCAS(kDoc1ID, 17, kBody1, t);
        Assert(rev != nullptr);
        AssertEqual(rev->docID(), kDoc1ID);
        AssertEqual(rev->body(), kBody1.body);
        AssertEqual(rev->version(), VersionVector(slice("17@$")));
        AssertEqual(rev->CAS(), 17ull);

        // Adding earlier CASs should do nothing:
        AssertNull(store->insertCAS(kDoc1ID, 17, kBody1, t).get());
        AssertNull(store->insertCAS(kDoc1ID, 10, kBody1, t).get());

        // Update to CAS=18:
        rev = store->insertCAS(kDoc1ID, 18, kBody2, t);
        Assert(rev != nullptr);
        AssertEqual(rev->docID(), kDoc1ID);
        AssertEqual(rev->body(), kBody2.body);
        AssertEqual(rev->version(), VersionVector(slice("18@$")));
        AssertEqual(rev->CAS(), 18ull);

        // Previous revision (17) shouldn't be around:
        AssertNull(store->get(kDoc1ID, slice("17@$")).get());

        // Latest version is 18:
        rev = store->getLatestCASServerRevision(kDoc1ID);
        Assert(rev != nullptr);
        AssertEqual(rev->version(), VersionVector(slice("18@$")));
        AssertEqual(rev->CAS(), 18ull);
    }


    void testAddLocalRevs() {
        // Start with CAS=18:
        Transaction t(db);
        auto rev = store->insertCAS(kDoc1ID, 18, kBody1, t);

        // Update it locally:
        rev = store->create(kDoc1ID, rev->version(), kBody2, t);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,18@$"));

        // Current revision is the local one:
        rev = store->get(kDoc1ID);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,18@$"));
        AssertEqual(rev->CAS(), 0ull);
        AssertEqual(rev->version().CASBase(), 18ull); // Based on CAS 18 from server

        // Latest CAS version is 18:
        auto casrev = store->getLatestCASServerRevision(kDoc1ID);
        Assert(casrev != nullptr);
        AssertEqual(casrev->version(), VersionVector(slice("18@$")));

        // Can get revision 18 by revID:
        Assert(store->get(kDoc1ID, slice("18@$")) != nullptr);

        // Adding same CAS again should do nothing:
        AssertNull(store->insertCAS(kDoc1ID, 17, kBody1, t).get());

        // Alright, now assume we PUT this to the server and it gets accepted as CAS 23.
        pushRev(*rev, t, 18, 23);

        rev = store->get(kDoc1ID);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*,18@$"));    // vvec hasn't changed
        AssertEqual(rev->CAS(), 23ull);                                     // but CAS has

        // Ancestor revision 18 is gone:
        AssertNull(store->get(kDoc1ID, slice("18@$")).get());
    }


    void pushRev(Revision &rev, Transaction &t, generation expectedBaseCAS, generation newCAS) {
        generation baseCAS = rev.version().CASBase();
        AssertEqual(baseCAS, expectedBaseCAS);
        // ... here the rev's body & baseCAS would be sent to the server, which would return newCAS
        store->assignCAS(rev, newCAS, t);
        AssertEqual(rev.CAS(), newCAS);
    }


    CPPUNIT_TEST_SUITE( CASRevisionStoreTest );
    CPPUNIT_TEST( testEmptyStore );
    CPPUNIT_TEST( testInsertCASRevs );
    CPPUNIT_TEST( testAddLocalRevs );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(CASRevisionStoreTest);
