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

    string getNames(FLMutableArray source) {
        stringstream result;
        delimiter delim(", ");
        FLArrayIterator i;
        FLArrayIterator_Begin(source, &i);
        if(FLArrayIterator_GetCount(&i) == 0) {
            return "";
        }

        do {
            string next = (string)(FLValue_AsString(FLArrayIterator_GetValue(&i)));
            result << delim << next;
        } while(FLArrayIterator_Next(&i));

        return result.str();
    }

    string getCollectionNames(slice inScope) {
        auto names = c4db_collectionNames(db, inScope, ERROR_INFO());
        REQUIRE(names);
        auto result = getNames(names);
        FLMutableArray_Release(names);
        return result;
    }


    string getScopeNames() {
        auto names = c4db_scopeNames(db, ERROR_INFO());
        REQUIRE(names);
        auto result = getNames(names);
        FLMutableArray_Release(names);
        return result;
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
            auto doc = c4coll_putDoc(coll, &rq, nullptr, ERROR_INFO());
            REQUIRE(doc);
            c4doc_release(doc);
        }
    }
};


static constexpr slice GuitarsName = "guitars"_sl;
static constexpr C4CollectionSpec Guitars = { GuitarsName, kC4DefaultScopeID };


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Default Collection", "[Database][Collection][C]") {
    CHECK(getScopeNames() == "_default");
    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");
    CHECK(c4db_hasCollection(db, kC4DefaultCollectionSpec));
    CHECK(c4db_hasScope(db, kC4DefaultScopeID));

    C4Collection* dflt = requireCollection(db);
    CHECK(dflt == c4db_getDefaultCollection(db, nullptr));              // Must be idempotent!
    CHECK(dflt == c4db_getCollection(db, kC4DefaultCollectionSpec, ERROR_INFO()));
    CHECK(dflt == c4db_createCollection(db, kC4DefaultCollectionSpec, ERROR_INFO()));
    
    auto spec = c4coll_getSpec(dflt);
    CHECK(spec.name == kC4DefaultCollectionName);
    CHECK(spec.scope == kC4DefaultScopeID);

    CHECK(c4coll_getDatabase(dflt) == db);
    CHECK(c4coll_getDocumentCount(dflt) == 0);
    CHECK(c4coll_getLastSequence(dflt) == 0_seq);
    // The existing c4Database tests exercise the C4Collection API for the default collection,
    // since they call the c4Database C functions which are wrappers that indirect through
    // db->getDefaultCollection().

    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");

    // It is, surprisingly, legal to delete the default collection:
    REQUIRE(c4db_deleteCollection(db, kC4DefaultCollectionSpec, ERROR_INFO()));
    C4Error err {};
    CHECK(c4db_getDefaultCollection(db, &err) == nullptr);
    CHECK(err.domain == 0);
    CHECK(err.code == 0);
    CHECK(c4db_getCollection(db, kC4DefaultCollectionSpec, ERROR_INFO()) == nullptr);
    CHECK(getCollectionNames(kC4DefaultScopeID) == "");
    
    // But you can't recreate it:
    c4log_warnOnErrors(false);
    CHECK(!c4db_createCollection(db, kC4DefaultCollectionSpec, &err));
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorInvalidParameter);
    c4log_warnOnErrors(true);

    // However, the default scope still exists
    CHECK(c4db_hasScope(db, kC4DefaultScopeID));
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Lifecycle", "[Database][Collection][C]") {
    CHECK(!c4db_hasCollection(db, Guitars));
    CHECK(c4db_getCollection(db, Guitars, ERROR_INFO()) == nullptr);

    // Create "guitars" collection:
    Retained<C4Collection> guitars = c4db_createCollection(db, Guitars, ERROR_INFO());
    REQUIRE(guitars);
    CHECK(guitars == c4db_getCollection(db, Guitars, ERROR_INFO()));

    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default, guitars");

    Retained<C4Collection> dflt = requireCollection(db);
    CHECK(dflt != guitars);

    // Put some stuff in the default collection
    createNumberedDocs(100);
    CHECK(c4coll_getDocumentCount(dflt) == 100);
    CHECK(c4coll_getLastSequence(dflt) == 100_seq);

    // Verify "guitars" is empty:
    auto spec = c4coll_getSpec(guitars);
    CHECK(spec.name == GuitarsName);
    CHECK(spec.scope == kC4DefaultScopeID);
    CHECK(c4coll_getDatabase(guitars) == db);
    CHECK(c4coll_getDocumentCount(guitars) == 0);
    CHECK(c4coll_getLastSequence(guitars) == 0_seq);

    // Delete "guitars":
    CHECK(c4coll_isValid(guitars));
    REQUIRE(c4db_deleteCollection(db, Guitars, ERROR_INFO()));
    CHECK(!c4coll_isValid(guitars));

    spec = c4coll_getSpec(guitars);
    CHECK(spec.name == GuitarsName);
    CHECK(spec.scope == kC4DefaultScopeID);

    //TODO: Expose error?
    CHECK(!c4coll_getDatabase(guitars));
    CHECK(c4coll_getDocumentCount(guitars) == 0); 
 
    CHECK(!c4db_hasCollection(db, Guitars));
    CHECK(c4db_getCollection(db, Guitars, ERROR_INFO()) == nullptr);
    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");

    // Close the database, then try to use the C4Collections:
    CHECK(c4coll_isValid(dflt));
    closeDB();
    CHECK(!c4coll_isValid(dflt));
    CHECK(!c4coll_isValid(guitars));
    
    CHECK(!c4coll_getDatabase(dflt));
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Lifecycle Multi-DB", "[Database][Collection][C]") {
    C4Database* db2 = c4db_openAgain(db, ERROR_INFO());
    REQUIRE(db2);


    Retained<C4Collection> guitars = c4db_createCollection(db, Guitars, ERROR_INFO());
    REQUIRE(guitars);

    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 100);
        t.commit();
    }

    Retained<C4Collection> guitars2 = c4db_getCollection(db2, Guitars, ERROR_INFO());
    REQUIRE(guitars2);

    CHECK(c4coll_getDocumentCount(guitars) == 100);
    CHECK(c4coll_getDocumentCount(guitars2) == 100);
    CHECK(c4coll_getLastSequence(guitars) == 100_seq);
    CHECK(c4coll_getLastSequence(guitars2) == 100_seq);

    // First delete the collection on the second DB instances
    REQUIRE(c4db_deleteCollection(db2, Guitars, ERROR_INFO()));
    CHECK(!c4coll_isValid(guitars2));
    CHECK(!c4coll_isValid(guitars));
    CHECK(!c4db_hasCollection(db2, Guitars));
    CHECK(!c4db_hasCollection(db, Guitars));
    CHECK(!c4db_getCollection(db2, Guitars, ERROR_INFO()));
    CHECK(!c4db_getCollection(db, Guitars, ERROR_INFO()));

    const C4Error notOpenError = { LiteCoreDomain, kC4ErrorNotOpen };
    C4Error err;
    CHECK(!c4coll_getDoc(guitars, "foo"_sl, false, kDocGetCurrentRev, &err));
    CHECK(err == notOpenError);
    CHECK(!c4coll_getDoc(guitars2, "foo"_sl, false, kDocGetCurrentRev, &err));
    CHECK(err == notOpenError);

    // Then recreate it on the first DB instance
    guitars = c4db_createCollection(db, Guitars, ERROR_INFO());
    REQUIRE(guitars);
    guitars2 = c4db_getCollection(db2, Guitars, ERROR_INFO());
    REQUIRE(guitars2);

    CHECK(c4coll_isValid(guitars2));
    CHECK(c4coll_isValid(guitars));
    CHECK(c4coll_getDocumentCount(guitars) == 0);
    CHECK(c4coll_getDocumentCount(guitars2) == 0);

    // NOTE: Sequence numbers do NOT reset in this case
    CHECK(c4coll_getLastSequence(guitars) == 100_seq);
    CHECK(c4coll_getLastSequence(guitars2) == 100_seq);
    CHECK(c4db_hasCollection(db2, Guitars));
    CHECK(c4db_hasCollection(db, Guitars));
    CHECK(c4db_getCollection(db2, Guitars, ERROR_INFO()));
    CHECK(c4db_getCollection(db, Guitars, ERROR_INFO()));

    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 100);
        t.commit();
    }

    CHECK(c4coll_getDocumentCount(guitars) == 100);
    CHECK(c4coll_getDocumentCount(guitars2) == 100);
    CHECK(c4coll_getLastSequence(guitars) == 200_seq);
    CHECK(c4coll_getLastSequence(guitars2) == 200_seq);

    c4db_close(db2, nullptr);
    c4db_release(db2);
}

N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Use after close", "[Database][Collection][C]") {
    REQUIRE(c4db_close(db, ERROR_INFO()));

    const C4Error notOpen { LiteCoreDomain, kC4ErrorNotOpen };
    C4Error err;
    CHECK(!c4db_getDefaultCollection(db, &err));
    CHECK(err == notOpen);

    err.code = 0;
    CHECK(!c4db_getCollection(db, Guitars, &err));
    CHECK(err == notOpen);

    err.code = 0;
    CHECK(!c4db_collectionNames(db, kC4DefaultScopeID, &err));
    CHECK(err == notOpen);

    err.code = 0;
    CHECK(!c4db_scopeNames(db, &err));
    CHECK(err == notOpen);

    err.code = 0;
    CHECK(!c4db_createCollection(db, Guitars, &err));
    CHECK(err == notOpen);

    err.code = 0;
    CHECK(!c4db_deleteCollection(db, Guitars, &err));
    CHECK(err == notOpen);
    
    c4db_release(db);
    db = nullptr;
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Create Docs", "[Database][Collection][C]") {
    // Create "guitars" collection:
    C4Collection* guitars = c4db_createCollection(db, Guitars, ERROR_INFO());
    REQUIRE(guitars);
    C4Collection* dflt = requireCollection(db);

    // Add 100 documents to it:
    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 100);
        t.commit();
    }

    CHECK(c4coll_getDocumentCount(guitars) == 100);
    CHECK(c4coll_getLastSequence(guitars) == 100_seq);
    CHECK(c4coll_getDocumentCount(dflt) == 0);
    CHECK(c4coll_getLastSequence(dflt) == 0_seq);

    // Add more docs to it and _default, but abort:
    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 100, 101);
        addNumberedDocs(dflt, 100);

        CHECK(c4coll_getDocumentCount(guitars) == 200);
        CHECK(c4coll_getLastSequence(guitars) == 200_seq);
        CHECK(c4coll_getDocumentCount(dflt) == 100);
        CHECK(c4coll_getLastSequence(dflt) == 100_seq);

        t.abort();
    }

    CHECK(c4coll_getDocumentCount(guitars) == 100);
    CHECK(c4coll_getLastSequence(guitars) == 100_seq);
    CHECK(c4coll_getDocumentCount(dflt) == 0);
    CHECK(c4coll_getLastSequence(dflt) == 0_seq);
}


N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Scopes", "[Database][Collection][C]") {
    static constexpr slice SupaDopeCollection = "fresh"_sl;
    static constexpr slice SupaDopeScope = "SupaDope"_sl;
    static constexpr C4CollectionSpec SupaDope = { SupaDopeCollection, SupaDopeScope };

    CHECK(getScopeNames() == "_default");
    CHECK(c4db_getCollection(db, SupaDope, ERROR_INFO()) == nullptr);
    C4Collection* fresh = c4db_createCollection(db, SupaDope, ERROR_INFO());
    REQUIRE(fresh);

    // Verify "fresh" is empty:
    auto spec = c4coll_getSpec(fresh);
    CHECK(spec.name == SupaDopeCollection);
    CHECK(spec.scope == SupaDopeScope);
    CHECK(c4coll_getDatabase(fresh) == db);
    CHECK(c4coll_getDocumentCount(fresh) == 0);
    CHECK(c4coll_getLastSequence(fresh) == 0_seq);
}
