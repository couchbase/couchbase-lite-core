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
#include "c4Test.hh"
#include "Delimiter.hh"
#include <sstream>

using namespace std;


// NOTE: This tests the public API, but it's not a C API (yet?), so this test has to be linked
// into CppTests, not C4Tests.


class C4CollectionTest : public C4Test {
public:

    C4CollectionTest(int testOption) :C4Test(testOption) { }

    string getCollectionNames(C4ScopeID inScope) {
        stringstream result;
        delimiter delim(", ");
        db->forEachCollection(inScope, [&](slice name) {
            result << delim << string_view(name);
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


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Default Collection", "[Database][Collection][C]") {
    CHECK(getScopeNames() == "_default");
    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");
    CHECK(db->hasCollection("_default", kC4DefaultScopeID));

    C4Collection* dflt = db->getDefaultCollection();
    REQUIRE(dflt);
    CHECK(dflt == db->getDefaultCollection());              // Must be idempotent!
    CHECK(dflt == db->getCollection("_default", kC4DefaultScopeID));

    CHECK(dflt->getName() == "_default");
    CHECK(dflt->getDatabase() == db);
    CHECK(dflt->getDocumentCount() == 0);
    CHECK(dflt->getLastSequence() == 0_seq);
    // The existing c4Database tests exercise the C4Collection API for the default collection,
    // since they call the c4Database C functions which are wrappers that indirect through
    // db->getDefaultCollection().

    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Lifecycle", "[Database][Collection][C]") {
    CHECK(!db->hasCollection("guitars", kC4DefaultScopeID));
    CHECK(db->getCollection("guitars", kC4DefaultScopeID) == nullptr);

    // Create "guitars" collection:
    C4Collection* guitars = db->createCollection("guitars", kC4DefaultScopeID);
    CHECK(guitars == db->getCollection("guitars", kC4DefaultScopeID));

    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default, guitars");

    C4Collection* dflt = db->getDefaultCollection();
    CHECK(dflt != guitars);

    // Put some stuff in the default collection
    createNumberedDocs(100);
    CHECK(dflt->getDocumentCount() == 100);
    CHECK(dflt->getLastSequence() == 100_seq);

    // Verify "guitars" is empty:
    CHECK(guitars->getName() == "guitars");
    CHECK(guitars->getDatabase() == db);
    CHECK(guitars->getDocumentCount() == 0);
    CHECK(guitars->getLastSequence() == 0_seq);

    db->deleteCollection("guitars", kC4DefaultScopeID);
    CHECK(!db->hasCollection("guitars", kC4DefaultScopeID));
    CHECK(db->getCollection("guitars", kC4DefaultScopeID) == nullptr);
    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Create Docs", "[Database][Collection][C]") {
    // Create "guitars" collection:
    C4Collection* guitars = db->createCollection("guitars", kC4DefaultScopeID);
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
