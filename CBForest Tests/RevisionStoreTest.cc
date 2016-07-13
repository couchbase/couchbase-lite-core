//
//  RevisionStoreTest.cc
//  CBForest
//
//  Created by Jens Alfke on 7/12/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "RevisionStore.hh"
#include "CBForestTest.hh"
#include <unistd.h>
using namespace cbforest;


static const slice kDoc1ID("Doc1");
static const slice kRev1ID("1@*");

static const Revision::BodyParams kBody1 {
    slice("{\"foo\":true}"), slice("foodoc"), false, false
};
static const Revision::BodyParams kBody2 {
    slice("{\"foo\":23,\"_attachments\":{}}"), slice("foodoc"), false, true
};


class RevisionStoreTest : public CppUnit::TestFixture {
public:
    
    Database *db {NULL};
    RevisionStore *store {NULL};

    void setUp() {
        TestFixture::setUp();
#ifdef _MSC_VER
        const char *dbPath = "C:\\tmp\\forest_temp.fdb";
        ::unlink("C:\\tmp\\forest_temp.fdb");
        ::unlink("C:\\tmp\\forest_temp.fdb.0");
        ::unlink("C:\\tmp\\forest_temp.fdb.1");
        ::unlink("C:\\tmp\\forest_temp.fdb.meta");
#else
        const char *dbPath = "/tmp/forest_temp.fdb";
        ::unlink("/tmp/forest_temp.fdb");
        ::unlink("/tmp/forest_temp.fdb.0");
        ::unlink("/tmp/forest_temp.fdb.1");
        ::unlink("/tmp/forest_temp.fdb.meta");
#endif
        db = new Database(dbPath, Database::defaultConfig());
        store = new RevisionStore(db);
    }

    void tearDown() {
        delete store;
        delete db;
        TestFixture::tearDown();
    }


    void testKeys() {
        AssertEqual(RevisionStore::keyForNonCurrentRevision(kDoc1ID, version(2, peerID("snej"))),
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


    CPPUNIT_TEST_SUITE( RevisionStoreTest );
    CPPUNIT_TEST( testKeys );
    CPPUNIT_TEST( testEmptyStore );
    CPPUNIT_TEST( testCreateRevs );
    CPPUNIT_TEST( testInsertRevs );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(RevisionStoreTest);
