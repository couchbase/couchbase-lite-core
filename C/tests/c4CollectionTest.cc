//
// c4CollectionTest.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "c4Collection.hh"
#include "c4Database.hh"
#include "c4Test.hh"

using namespace std;


// NOTE: This tests the public API, but it's not a C API (yet?), so this test has to be linked
// into CppTests, not C4Tests.


class C4CollectionTest : public C4Test {
public:

    C4CollectionTest(int testOption) :C4Test(testOption) { }

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
    CHECK(db->getCollectionNames() == (vector<string>{"_default"}));
    CHECK(db->hasCollection("_default"));

    C4Collection* dflt = db->getDefaultCollection();
    REQUIRE(dflt);
    CHECK(dflt == db->getDefaultCollection());              // Must be idempotent!
    CHECK(dflt == db->getCollection("_default"));

    CHECK(dflt->getName() == "_default");
    CHECK(dflt->getDatabase() == db);
    CHECK(dflt->getDocumentCount() == 0);
    CHECK(dflt->getLastSequence() == 0);
    // The existing c4Database tests exercise the C4Collection API for the default collection,
    // since they call the c4Database C functions which are wrappers that indirect through
    // db->getDefaultCollection().

    vector<C4Collection*> eachCollection;
    db->forEachCollection([&](C4Collection *coll) { eachCollection.push_back(coll); });
    CHECK(eachCollection.size() == 1);
    CHECK(eachCollection[0] == dflt);
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Lifecycle", "[Database][Collection][C]") {
    CHECK(!db->hasCollection("guitars"));
    CHECK(db->getCollection("guitars") == nullptr);

    // Create "guitars" collection:
    C4Collection* guitars = db->createCollection("guitars");
    CHECK(guitars == db->getCollection("guitars"));

    CHECK(db->getCollectionNames() == (vector<string>{"_default", "guitars"}));

    C4Collection* dflt = db->getDefaultCollection();
    CHECK(dflt != guitars);

    vector<C4Collection*> eachCollection;
    db->forEachCollection([&](C4Collection *coll) { eachCollection.push_back(coll); });
    CHECK(eachCollection.size() == 2);
    CHECK(eachCollection[0] == dflt);
    CHECK(eachCollection[1] == guitars);

    // Put some stuff in the default collection
    createNumberedDocs(100);
    CHECK(dflt->getDocumentCount() == 100);
    CHECK(dflt->getLastSequence() == 100);

    // Verify "guitars" is empty:
    CHECK(guitars->getName() == "guitars");
    CHECK(guitars->getDatabase() == db);
    CHECK(guitars->getDocumentCount() == 0);
    CHECK(guitars->getLastSequence() == 0);

    db->deleteCollection("guitars");
    CHECK(!db->hasCollection("guitars"));
    CHECK(db->getCollection("guitars") == nullptr);
    CHECK(db->getCollectionNames() == (vector<string>{"_default"}));
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Create Docs", "[Database][Collection][C]") {
    // Create "guitars" collection:
    C4Collection* guitars = db->createCollection("guitars");
    C4Collection* dflt = db->getDefaultCollection();

    // Add 100 documents to it:
    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 100);
        t.commit();
    }
    CHECK(guitars->getDocumentCount() == 100);
    CHECK(guitars->getLastSequence() == 100);
    CHECK(dflt->getDocumentCount() == 0);
    CHECK(dflt->getLastSequence() == 0);

    // Add more docs to it and _default, but abort:
    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 100, 101);
        addNumberedDocs(dflt, 100);

        CHECK(guitars->getDocumentCount() == 200);
        CHECK(guitars->getLastSequence() == 200);
        CHECK(dflt->getDocumentCount() == 100);
        CHECK(dflt->getLastSequence() == 100);

        t.abort();
    }
    CHECK(guitars->getDocumentCount() == 100);
    CHECK(guitars->getLastSequence() == 100);
    CHECK(dflt->getDocumentCount() == 0);
    CHECK(dflt->getLastSequence() == 0);
}
