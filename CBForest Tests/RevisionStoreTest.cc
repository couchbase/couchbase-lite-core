//
//  RevisionStoreTest.cc
//  CBForest
//
//  Created by Jens Alfke on 7/12/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "RevisionStore.hh"
#include "CBForestTest.hh"
#include "LogInternal.hh"


static const slice kDoc1ID("Doc1");
static const slice kRev1ID("1@*");

static const Revision::BodyParams kBody1 {
    slice("{\"foo\":true}"), slice("foodoc"), false, false
};
static const Revision::BodyParams kBody2 {
    slice("{\"foo\":23,\"_attachments\":{}}"), slice("foodoc"), false, true
};
static const Revision::BodyParams kBody3 {
    slice("{\"foo\":24,\"_attachments\":{}}"), slice("foodoc"), false, true
};
static const Revision::BodyParams kBody4 {
    slice("{\"foo\":25,\"_attachments\":{}}"), slice("foodoc"), false, true
};


class RevisionStoreTest : public DatabaseTestFixture {
public:
    
    RevisionStore *store {NULL};

    void setUp() {
        DatabaseTestFixture::setUp();
        store = new RevisionStore(db, peerID("jens"));
    }

    void tearDown() {
        delete store;
        DatabaseTestFixture::tearDown();
    }


    void testKeys() {
        AssertEqual(RevisionStore::keyForNonCurrentRevision(kDoc1ID, Version(2, peerID("snej"))),
                    alloc_slice("Doc1\tsnej,\002"));
        AssertEqual(RevisionStore::startKeyFor(kDoc1ID, peerID("snej")),
                    alloc_slice("Doc1\tsnej,"));
        AssertEqual(RevisionStore::endKeyFor(kDoc1ID, peerID("snej")),
                    alloc_slice("Doc1\tsnej-"));
        AssertEqual(RevisionStore::docIDFromKey(slice("Doc1\tsnej,\002")),
                    slice("Doc1"));
    }


    void testEmptyStore() {
        Assert(store->get(kDoc1ID) == nullptr);
        Assert(store->get(kDoc1ID, kRev1ID) == nullptr);
        AssertEqual(store->checkRevision(kDoc1ID, kRev1ID), kOlder);
    }

    
    void testCreateRevs() {
        // Create a new document:
        Transaction t(db);
        auto rev = store->create(kDoc1ID, VersionVector(), kBody1, t);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("1@*"));

        // Read it back:
        auto gotRev = store->get(kDoc1ID);
        Assert(gotRev);
        AssertEqual(gotRev->docID(), kDoc1ID);
        AssertEqual(gotRev->version().asString(), std::string("1@*"));
        AssertEqual(gotRev->body(), rev->body());

        // Try to create a conflict, and fail:
        auto conflict = store->create(kDoc1ID, VersionVector(), kBody1, t);
        AssertNull(conflict.get());

        // Create a second revision:
        rev = store->create(kDoc1ID, rev->version(), kBody2, t);
        Assert(rev);
        AssertEqual(rev->version().asString(), std::string("2@*"));

        // Read it back:
        gotRev = store->get(kDoc1ID);
        Assert(gotRev);
        AssertEqual(gotRev->version().asString(), std::string("2@*"));
        AssertEqual(gotRev->body(), rev->body());
        Assert(gotRev->hasAttachments());

        // First revision shouldn't still exist:
        Assert(store->get(kDoc1ID, kRev1ID) == nullptr);
        AssertEqual(store->checkRevision(kDoc1ID, kRev1ID), kOlder);
        AssertEqual(store->checkRevision(kDoc1ID, slice("2@*")), kSame);
        AssertEqual(store->checkRevision(kDoc1ID, slice("1@bob")), kNewer);
    }


    void testInsertRevs() {
        Transaction t(db);
        Revision rev1(kDoc1ID, VersionVector(slice("5@bob,1@ada")),
                      Revision::BodyParams{kBody1}, true);
        AssertEqual(store->insert(rev1, t), kNewer);

        Revision rev2(kDoc1ID, VersionVector(slice("4@bob")),
                      Revision::BodyParams{kBody1}, true);
        AssertEqual(store->insert(rev2, t), kOlder);

        Revision rev3(kDoc1ID, VersionVector(slice("1@ada")),
                      Revision::BodyParams{kBody1}, true);
        AssertEqual(store->insert(rev3, t), kOlder);

        // Newer revision by another author:
        Revision rev4(kDoc1ID, VersionVector(slice("2@ada,5@bob")),
                      Revision::BodyParams{kBody1}, true);
        AssertEqual(store->insert(rev4, t), kNewer);

        auto gotRev = store->get(kDoc1ID, slice("2@ada"));
        Assert(gotRev);
        AssertEqual(gotRev->version().asString(), std::string("2@ada,5@bob"));

        AssertEqual(store->checkRevision(kDoc1ID, slice("5@bob")), kOlder);
        AssertEqual(store->checkRevision(kDoc1ID, slice("1@ada")), kOlder);
        AssertEqual(store->checkRevision(kDoc1ID, slice("2@ada")), kSame);
        AssertEqual(store->checkRevision(kDoc1ID, slice("3@ada")), kNewer);
        AssertEqual(store->checkRevision(kDoc1ID, slice("6@bob")), kNewer);
        AssertEqual(store->checkRevision(kDoc1ID, slice("1@tim")), kNewer);
    }


    void testConflict() {
        //LogLevel = kDebug;
        // Start with a doc edited by me and Ada:
        Transaction t(db);
        Revision rev1(kDoc1ID, VersionVector(slice("5@*,1@ada")),
                      Revision::BodyParams{kBody1}, true);
        AssertEqual(store->insert(rev1, t), kNewer);

        // Update it locally:
        auto myRev = store->create(kDoc1ID, rev1.version(), kBody2, t);
        Assert(myRev);
        AssertEqual(myRev->version().asString(), std::string("6@*,1@ada"));

        // Ada updates the original doc too:
        Revision revC(kDoc1ID, VersionVector(slice("2@ada,5@*")),
                      Revision::BodyParams{kBody3}, true);
        AssertEqual(store->insert(revC, t), kConflicting);

        // Check that we can get both my rev and the conflicting one:
        auto current = store->get(kDoc1ID);
        Assert(current.get());
        AssertEqual(current->version(), myRev->version());
        Assert(current->isConflicted());
        auto getRevC = store->get(kDoc1ID, revC.revID());
        Assert(getRevC.get());
        AssertEqual(getRevC->version(), revC.version());
        Assert(getRevC->isConflicted());

        std::vector<Revision*> conflicts = {current.get(), getRevC.get()};
        auto resolved = store->resolveConflict(conflicts, kBody4, t);
        Assert(resolved.get());
        // Note: Any change to the resolved revision's body, or to the digest algorithm,
        // will cause these assertions to fail:
        AssertEqual(resolved->version().asString(), std::string("^sHsohHU0KoR+wvwbc5jjJgtA56Q=,6@*,2@ada"));
        AssertEqual(resolved->revID().asString(), std::string("^sHsohHU0KoR+wvwbc5jjJgtA56Q="));

        auto getResolved = store->get(kDoc1ID);
        Assert(getResolved.get());
        AssertEqual(getResolved->version(), resolved->version());
        Assert(!getResolved->isConflicted());

        Assert(store->get(kDoc1ID, current->revID()).get() == nullptr);
        Assert(store->get(kDoc1ID, getRevC->revID()).get() == nullptr);
}


    CPPUNIT_TEST_SUITE( RevisionStoreTest );
    CPPUNIT_TEST( testKeys );
    CPPUNIT_TEST( testEmptyStore );
    CPPUNIT_TEST( testCreateRevs );
    CPPUNIT_TEST( testInsertRevs );
    CPPUNIT_TEST( testConflict );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(RevisionStoreTest);
