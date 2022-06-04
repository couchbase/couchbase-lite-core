//
// c4CollectionTest.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Collection.hh"
#include "c4Database.hh"
#include "c4Collection.h"
#include "c4Query.h"
#include "c4Test.hh"
#include "Delimiter.hh"
#include <sstream>

using namespace std;


// NOTE: This tests the public API, but it's not a C API (yet?), so this test has to be linked
// into CppTests, not C4Tests.


class C4CollectionTest : public C4Test {
public:

    C4CollectionTest(int testOption) :C4Test(testOption) { }

    string getCollectionNames(slice inScope) {
        stringstream result;
        delimiter delim(", ");
        db->forEachCollection(inScope, [&](C4CollectionSpec spec) {
            CHECK(spec.scope == inScope);
            result << delim << string_view(slice(spec.name));
        });
        return result.str();
    }


    string getScopeNames() {
        stringstream result;
        delimiter delim(", ");
        db->forEachScope([&](slice name) {
            result << delim << string_view(name);
        });
        return result.str();
    }


    void addNumberedDocs(C4Collection *coll, unsigned n, unsigned start = 1) {
        for (unsigned i = 0; i < n; i++) {
            char docID[20];
            sprintf(docID, "doc-%03u", start + i);
            C4String history[1] = {kRev1ID};
            C4DocPutRequest rq = {};
            rq.existingRevision = true;
            rq.docID = slice(docID);
            rq.history = history;
            rq.historyCount = 1;
            rq.body = kFleeceBody;
            rq.save = true;
            auto doc = coll->putDocument(rq, nullptr, ERROR_INFO());
            REQUIRE(doc);
            CHECK(doc->collection() == coll);
            CHECK(doc->database() == db);
        }
    }
};


static constexpr slice Guitars = "guitars"_sl;


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Default Collection", "[Database][Collection][C]") {
    CHECK(getScopeNames() == "_default");
    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");
    CHECK(db->hasCollection({kC4DefaultCollectionName, kC4DefaultScopeID}));

    C4Collection* dflt = db->getDefaultCollection();
    REQUIRE(dflt);
    CHECK(dflt == db->getDefaultCollection());              // Must be idempotent!
    CHECK(dflt == db->getCollection({kC4DefaultCollectionName, kC4DefaultScopeID}));
    CHECK(dflt == db->createCollection({kC4DefaultCollectionName, kC4DefaultScopeID}));

    CHECK(dflt->getName() == kC4DefaultCollectionName);
    CHECK(dflt->getScope() == kC4DefaultCollectionName);
    CHECK(dflt->getSpec().name == kC4DefaultCollectionName);
    CHECK(dflt->getSpec().scope == kC4DefaultCollectionName);

    CHECK(dflt->getDatabase() == db);
    CHECK(dflt->getDocumentCount() == 0);
    CHECK(dflt->getLastSequence() == 0_seq);
    // The existing c4Database tests exercise the C4Collection API for the default collection,
    // since they call the c4Database C functions which are wrappers that indirect through
    // db->getDefaultCollection().

    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");

    // It is, surprisingly, legal to delete the default collection:
    db->deleteCollection(kC4DefaultCollectionName);
    CHECK(db->getDefaultCollection() == nullptr);
    CHECK(db->getCollection(kC4DefaultCollectionName) == nullptr);
    CHECK(getCollectionNames(kC4DefaultScopeID) == "");
    // But you can't recreate it:
    C4ExpectException(LiteCoreDomain, kC4ErrorInvalidParameter, [&]{
        db->createCollection(kC4DefaultCollectionName);
    });

    // However, the default scope still exists
    CHECK(c4db_hasScope(db, kC4DefaultScopeID));
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Lifecycle", "[Database][Collection][C]") {
    CHECK(!db->hasCollection(Guitars));
    CHECK(db->getCollection(Guitars) == nullptr);

    // Create "guitars" collection:
    Retained<C4Collection> guitars = db->createCollection(Guitars);
    CHECK(guitars == db->getCollection(Guitars));

    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default, guitars");

    Retained<C4Collection> dflt = db->getDefaultCollection();
    CHECK(dflt != guitars);

    // Put some stuff in the default collection
    createNumberedDocs(100);
    CHECK(dflt->getDocumentCount() == 100);
    CHECK(dflt->getLastSequence() == 100_seq);

    // Verify "guitars" is empty:
    CHECK(guitars->getSpec().name == Guitars);
    CHECK(guitars->getSpec().scope == kC4DefaultScopeID);
    CHECK(guitars->getDatabase() == db);
    CHECK(guitars->getDocumentCount() == 0);
    CHECK(guitars->getLastSequence() == 0_seq);

    // Delete "guitars":
    CHECK(guitars->isValid());
    db->deleteCollection(Guitars);
    CHECK(!guitars->isValid());
    CHECK(guitars->getSpec().name == Guitars);
    CHECK(guitars->getSpec().scope == kC4DefaultScopeID);
    C4ExpectException(LiteCoreDomain, kC4ErrorNotOpen, [&]{
        guitars->getDatabase();
    });
    C4ExpectException(LiteCoreDomain, kC4ErrorNotOpen, [&]{
        guitars->getDocumentCount();
    });

    CHECK(!db->hasCollection(Guitars));
    CHECK(db->getCollection(Guitars) == nullptr);
    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");
    
    // Create collection with the same name of the collection that was deleted.
    Retained<C4Collection> guitarsAgain = db->createCollection(Guitars);
    CHECK(guitarsAgain == db->getCollection(Guitars));

    // Close the database, then try to use the C4Collections:
    CHECK(dflt->isValid());
    closeDB();
    CHECK(!dflt->isValid());
    CHECK(!guitars->isValid());
    CHECK(!guitarsAgain->isValid());

    C4ExpectException(LiteCoreDomain, kC4ErrorNotOpen, [&]{
        dflt->getDatabase();
    });
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Removal", "[Database][Collection][C]") {
    CHECK(!db->hasCollection(Guitars));
    CHECK(db->getCollection(Guitars) == nullptr);

    Retained<C4Database> db2 = db->openAgain();
    REQUIRE(db2);
    
    // Create "guitars" collection from db
    Retained<C4Collection> guitars = db->createCollection(Guitars);
    REQUIRE(guitars == db->getCollection(Guitars));

    // Create "guitars" collection from db2
    Retained<C4Collection> guitars2 = db2->getCollection(Guitars);
    REQUIRE((guitars2 && guitars2->isValid()));
    
    // Delete "guitars" from 2nd db instance and check isValid on both collection objects
    db2->deleteCollection(Guitars);
    CHECK(!guitars2->isValid());
    CHECK(!guitars->isValid());
    
    // Recreate it on the first db instance:
    guitars = db->createCollection(Guitars);
    REQUIRE(guitars);
    guitars2 = db2->getCollection(Guitars);
    CHECK(guitars2);
    CHECK(guitars->isValid());
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Create Docs", "[Database][Collection][C]") {
    // Create "guitars" collection:
    C4Collection* guitars = db->createCollection(Guitars);
    C4Collection* dflt = db->getDefaultCollection();

    // Add 100 documents to it:
    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 100);
        t.commit();
    }
    CHECK(guitars->getDocumentCount() == 100);
    CHECK(guitars->getLastSequence() == 100_seq);
    CHECK(dflt->getDocumentCount() == 0);
    CHECK(dflt->getLastSequence() == 0_seq);

    // Add more docs to it and _default, but abort:
    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 100, 101);
        addNumberedDocs(dflt, 100);

        CHECK(guitars->getDocumentCount() == 200);
        CHECK(guitars->getLastSequence() == 200_seq);
        CHECK(dflt->getDocumentCount() == 100);
        CHECK(dflt->getLastSequence() == 100_seq);

        t.abort();
    }
    CHECK(guitars->getDocumentCount() == 100);
    CHECK(guitars->getLastSequence() == 100_seq);
    CHECK(dflt->getDocumentCount() == 0);
    CHECK(dflt->getLastSequence() == 0_seq);
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Scopes", "[Database][Collection][C]") {
    static constexpr slice SupaDopeScope = "SupaDope";

    CHECK(getScopeNames() == "_default");
    CHECK(db->getCollection({"fresh"_sl, SupaDopeScope}) == nullptr);
    C4Collection* fresh = db->createCollection({"fresh"_sl, SupaDopeScope});
    REQUIRE(fresh);

    // Verify "fresh" is empty:
    CHECK(fresh->getSpec().name == "fresh"_sl);
    CHECK(fresh->getSpec().scope == SupaDopeScope);
    CHECK(fresh->getDatabase() == db);
    CHECK(fresh->getDocumentCount() == 0);
    CHECK(fresh->getLastSequence() == 0_seq);
}
