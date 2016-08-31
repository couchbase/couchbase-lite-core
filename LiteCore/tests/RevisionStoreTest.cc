//
//  RevisionStoreTest.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/12/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#include "RevisionStore.hh"
#include "LiteCoreTest.hh"
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


class RevisionStoreTest : public DataFileTestFixture {
public:
    
    RevisionStore *store {NULL};

    RevisionStoreTest(int testOption)
    :DataFileTestFixture(testOption)
    {
        store = new RevisionStore(db, peerID("jens"));
    }

    ~RevisionStoreTest() {
        delete store;
    }

    // This has to be a method of RevisionStoreTest, not a test case method, because it calls
    // non-public methods of RevisionStore, and RevisionStore only grants 'friend' access to
    // RevisionStoreTest itself, not the anonymous subclasses created by test methods.
    void testKeys() {
        REQUIRE(RevisionStore::keyForNonCurrentRevision(kDoc1ID, Version(2, peerID("snej"))) == alloc_slice("Doc1\tsnej,\002"));
        REQUIRE(RevisionStore::startKeyFor(kDoc1ID, peerID("snej")) == alloc_slice("Doc1\tsnej,"));
        REQUIRE(RevisionStore::endKeyFor(kDoc1ID, peerID("snej")) == alloc_slice("Doc1\tsnej-"));
        REQUIRE(RevisionStore::docIDFromKey(slice("Doc1\tsnej,\002")) == slice("Doc1"));
    }
};


N_WAY_TEST_CASE_METHOD(RevisionStoreTest, "Keys", "[RevisionStore]") {
    testKeys();
}


N_WAY_TEST_CASE_METHOD(RevisionStoreTest, "EmptyStore", "[RevisionStore]") {
    REQUIRE(store->get(kDoc1ID) == nullptr);
    REQUIRE(store->get(kDoc1ID, kRev1ID) == nullptr);
    REQUIRE(store->checkRevision(kDoc1ID, kRev1ID) == kOlder);
}


N_WAY_TEST_CASE_METHOD(RevisionStoreTest, "CreateRevs", "[RevisionStore]") {
    // Create a new document:
    Transaction t(db);
    auto rev = store->create(kDoc1ID, VersionVector(), kBody1, t);
    REQUIRE(rev);
    REQUIRE(rev->version().asString() == std::string("1@*"));

    // Read it back:
    auto gotRev = store->get(kDoc1ID);
    REQUIRE(gotRev);
    REQUIRE(gotRev->docID() == kDoc1ID);
    REQUIRE(gotRev->version().asString() == std::string("1@*"));
    REQUIRE(gotRev->body() == rev->body());

    // Try to create a conflict, and fail:
    auto conflict = store->create(kDoc1ID, VersionVector(), kBody1, t);
    REQUIRE(conflict.get() == nullptr);

    // Create a second revision:
    rev = store->create(kDoc1ID, rev->version(), kBody2, t);
    REQUIRE(rev);
    REQUIRE(rev->version().asString() == std::string("2@*"));

    // Read it back:
    gotRev = store->get(kDoc1ID);
    REQUIRE(gotRev);
    REQUIRE(gotRev->version().asString() == std::string("2@*"));
    REQUIRE(gotRev->body() == rev->body());
    REQUIRE(gotRev->hasAttachments());

    // First revision shouldn't still exist:
    REQUIRE(store->get(kDoc1ID, kRev1ID) == nullptr);
    REQUIRE(store->checkRevision(kDoc1ID, kRev1ID) == kOlder);
    REQUIRE(store->checkRevision(kDoc1ID, slice("2@*")) == kSame);
    REQUIRE(store->checkRevision(kDoc1ID, slice("1@bob")) == kNewer);
}


N_WAY_TEST_CASE_METHOD(RevisionStoreTest, "InsertRevs", "[RevisionStore]") {
    Transaction t(db);
    Revision rev1(kDoc1ID, VersionVector(slice("5@bob,1@ada")),
                  Revision::BodyParams{kBody1}, true);
    REQUIRE(store->insert(rev1, t) == kNewer);

    Revision rev2(kDoc1ID, VersionVector(slice("4@bob")),
                  Revision::BodyParams{kBody1}, true);
    REQUIRE(store->insert(rev2, t) == kOlder);

    Revision rev3(kDoc1ID, VersionVector(slice("1@ada")),
                  Revision::BodyParams{kBody1}, true);
    REQUIRE(store->insert(rev3, t) == kOlder);

    // Newer revision by another author:
    Revision rev4(kDoc1ID, VersionVector(slice("2@ada,5@bob")),
                  Revision::BodyParams{kBody1}, true);
    REQUIRE(store->insert(rev4, t) == kNewer);

    auto gotRev = store->get(kDoc1ID, slice("2@ada"));
    REQUIRE(gotRev);
    REQUIRE(gotRev->version().asString() == std::string("2@ada,5@bob"));

    REQUIRE(store->checkRevision(kDoc1ID, slice("5@bob")) == kOlder);
    REQUIRE(store->checkRevision(kDoc1ID, slice("1@ada")) == kOlder);
    REQUIRE(store->checkRevision(kDoc1ID, slice("2@ada")) == kSame);
    REQUIRE(store->checkRevision(kDoc1ID, slice("3@ada")) == kNewer);
    REQUIRE(store->checkRevision(kDoc1ID, slice("6@bob")) == kNewer);
    REQUIRE(store->checkRevision(kDoc1ID, slice("1@tim")) == kNewer);
}


N_WAY_TEST_CASE_METHOD(RevisionStoreTest, "Conflict", "[RevisionStore]") {
    //LogLevel = kDebug;
    // Start with a doc edited by me and Ada:
    Transaction t(db);
    Revision rev1(kDoc1ID, VersionVector(slice("5@*,1@ada")),
                  Revision::BodyParams{kBody1}, true);
    REQUIRE(store->insert(rev1, t) == kNewer);

    // Update it locally:
    auto myRev = store->create(kDoc1ID, rev1.version(), kBody2, t);
    REQUIRE(myRev);
    REQUIRE(myRev->version().asString() == std::string("6@*,1@ada"));

    // Ada updates the original doc too:
    Revision revC(kDoc1ID, VersionVector(slice("2@ada,5@*")),
                  Revision::BodyParams{kBody3}, true);
    REQUIRE(store->insert(revC, t) == kConflicting);

    // Check that we can get both my rev and the conflicting one:
    auto current = store->get(kDoc1ID);
    REQUIRE(current.get());
    REQUIRE(current->version() == myRev->version());
    REQUIRE(current->isConflicted());
    auto getRevC = store->get(kDoc1ID, revC.revID());
    REQUIRE(getRevC.get());
    REQUIRE(getRevC->version() == revC.version());
    REQUIRE(getRevC->isConflicted());

    std::vector<Revision*> conflicts = {current.get(), getRevC.get()};
    auto resolved = store->resolveConflict(conflicts, kBody4, t);
    REQUIRE(resolved.get());
    // Note: Any change to the resolved revision's body, or to the digest algorithm,
    // will cause these assertions to fail:
    REQUIRE(resolved->version().asString() == std::string("^sHsohHU0KoR+wvwbc5jjJgtA56Q=,6@*,2@ada"));
    REQUIRE(resolved->revID().asString() == std::string("^sHsohHU0KoR+wvwbc5jjJgtA56Q="));

    auto getResolved = store->get(kDoc1ID);
    REQUIRE(getResolved.get());
    REQUIRE(getResolved->version() == resolved->version());
    REQUIRE_FALSE(getResolved->isConflicted());

    REQUIRE(store->get(kDoc1ID, current->revID()).get() == nullptr);
    REQUIRE(store->get(kDoc1ID, getRevC->revID()).get() == nullptr);
}
