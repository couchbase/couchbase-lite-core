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
#include "c4Test.hh"  // IWYU pragma: keep
#include "Delimiter.hh"
#include <sstream>
#include <thread>

using namespace std;

static bool docExists(C4Collection* coll, slice docID) {
    C4Error err;
    auto    doc = c4::make_ref(c4coll_getDoc(coll, docID, true, kDocGetMetadata, &err));
    if ( doc ) return true;
    CHECK(err == C4Error{LiteCoreDomain, kC4ErrorNotFound});
    return false;
};

class C4CollectionTest : public C4Test {
  public:
    explicit C4CollectionTest(int testOption) : C4Test(testOption) {}

    static string getNames(FLMutableArray source) {
        stringstream    result;
        delimiter       delim(", ");
        FLArrayIterator i;
        FLArrayIterator_Begin(source, &i);
        if ( FLArrayIterator_GetCount(&i) == 0 ) { return ""; }

        do {
            string next = (string)(FLValue_AsString(FLArrayIterator_GetValue(&i)));
            result << delim << next;
        } while ( FLArrayIterator_Next(&i) );

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

    void addNumberedDocs(C4Collection* coll, unsigned n, unsigned start = 1) {
        constexpr size_t bufSize = 20;
        for ( unsigned i = 0; i < n; i++ ) {
            char docID[bufSize];
            snprintf(docID, bufSize, "doc-%03u", start + i);
            C4String        history[1] = {kRev1ID};
            C4DocPutRequest rq         = {};
            rq.existingRevision        = true;
            rq.docID                   = slice(docID);
            rq.history                 = history;
            rq.historyCount            = 1;
            rq.body                    = kFleeceBody;
            rq.save                    = true;
            auto doc                   = c4coll_putDoc(coll, &rq, nullptr, ERROR_INFO());
            REQUIRE(doc);
            c4doc_release(doc);
        }
    }
};

static constexpr slice            GuitarsName = "guitars"_sl;
static constexpr C4CollectionSpec Guitars     = {GuitarsName, kC4DefaultScopeID};

// CBL-3979: due to the technicality of SGW, we temporarily disallow deletion of the default
// collection
#define NOT_DELETE_DEFAULT_COLLECTION

N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Default Collection", "[Database][Collection][C]") {
    CHECK(getScopeNames() == "_default");
    CHECK(getCollectionNames(kC4DefaultScopeID) == "_default");
    CHECK(c4db_hasCollection(db, kC4DefaultCollectionSpec));
    CHECK(c4db_hasScope(db, kC4DefaultScopeID));

    C4Collection* dflt = requireCollection(db);
    CHECK(dflt == c4db_getDefaultCollection(db, nullptr));  // Must be idempotent!
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

    C4Error err{};

#ifndef NOT_DELETE_DEFAULT_COLLECTION

    // It is, surprisingly, legal to delete the default collection:
    REQUIRE(c4db_deleteCollection(db, kC4DefaultCollectionSpec, ERROR_INFO()));

    CHECK(c4db_getDefaultCollection(db, &err) == nullptr);
    CHECK(err.domain == 0);
    CHECK(err.code == 0);
    CHECK(c4db_getCollection(db, kC4DefaultCollectionSpec, ERROR_INFO()) == nullptr);
    CHECK(getCollectionNames(kC4DefaultScopeID) == "");

    // But you can't recreate it:
    c4log_warnOnErrors(false);
    ++gC4ExpectExceptions;
    CHECK(!c4db_createCollection(db, kC4DefaultCollectionSpec, &err));
    --gC4ExpectExceptions;
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorInvalidParameter);
    c4log_warnOnErrors(true);

    // However, the default scope still exists,
    CHECK(c4db_hasScope(db, kC4DefaultScopeID));
    // and scopeNames should include the default scope as well.
    FLMutableArray names = c4db_scopeNames(db, ERROR_INFO());
    CHECK(FLArray_Count(names) == 1);
    FLValue name = FLArray_Get(names, 0);
    CHECK(FLSlice_Compare(FLValue_AsString(name), kC4DefaultScopeID) == 0);
    FLMutableArray_Release(names);

#else

    // It is illegal to delete the default collection:
    {
        ExpectingExceptions x;
        REQUIRE(!c4db_deleteCollection(db, kC4DefaultCollectionSpec, &err));
    }
    CHECK((err.domain == LiteCoreDomain && err.code == kC4ErrorInvalidParameter));
    C4SliceResult         errMsg         = c4error_getMessage(err);
    constexpr const char* expectedErrMsg = "Default collection cannot be deleted.";
    CHECK(std::string((char*)errMsg.buf, errMsg.size) == expectedErrMsg);
    c4slice_free(errMsg);
#endif
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
    ++gC4ExpectExceptions;
    CHECK(c4coll_getDocumentCount(guitars) == 0);
    --gC4ExpectExceptions;

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

    ++gC4ExpectExceptions;
    const C4Error notOpenError = {LiteCoreDomain, kC4ErrorNotOpen};
    C4Error       err;
    CHECK(!c4coll_getDoc(guitars, "foo"_sl, false, kDocGetCurrentRev, &err));
    CHECK(err == notOpenError);
    CHECK(!c4coll_getDoc(guitars2, "foo"_sl, false, kDocGetCurrentRev, &err));
    CHECK(err == notOpenError);
    --gC4ExpectExceptions;

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

    ++gC4ExpectExceptions;

    const C4Error notOpen{LiteCoreDomain, kC4ErrorNotOpen};
    C4Error       err;
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

    --gC4ExpectExceptions;

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

static constexpr slice            SupaDopeCollection = "fresh"_sl;
static constexpr slice            SupaDopeScope      = "SupaDope"_sl;
static constexpr C4CollectionSpec SupaDope           = {SupaDopeCollection, SupaDopeScope};

N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Scopes", "[Database][Collection][C]") {
    CHECK(getScopeNames() == "_default");
    CHECK(c4db_getCollection(db, SupaDope, ERROR_INFO()) == nullptr);
    C4Collection* fresh = c4db_createCollection(db, SupaDope, ERROR_INFO());
    REQUIRE(fresh);

    // CBL-3298
    C4Database* db2 = c4db_openAgain(db, ERROR_INFO());
    REQUIRE(db2);
    CHECK(c4db_hasScope(db2, SupaDopeScope));
    c4db_close(db2, nullptr);
    c4db_release(db2);

    // Verify "fresh" is empty:
    auto spec = c4coll_getSpec(fresh);
    CHECK(spec.name == SupaDopeCollection);
    CHECK(spec.scope == SupaDopeScope);
    CHECK(c4coll_getDatabase(fresh) == db);
    CHECK(c4coll_getDocumentCount(fresh) == 0);
    CHECK(c4coll_getLastSequence(fresh) == 0_seq);
}

N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Collection Expired", "[Collection][C][Expiration]") {
    // With this test, we can explicitly ensure that expiration works on a named collection.
    // However, aside from this facet, the other facets of expiration are tested
    // via c4DatabaseTest expiration, which uses the default collection.
    const C4Error notFound = {LiteCoreDomain, kC4ErrorNotFound};
    C4Collection* fresh    = c4db_createCollection(db, SupaDope, ERROR_INFO());
    REQUIRE(fresh);

    C4Error err;
    CHECK(c4coll_nextDocExpiration(fresh) == C4Timestamp::None);
    CHECK(c4coll_purgeExpiredDocs(fresh, WITH_ERROR()) == 0);

    C4Slice docID = C4STR("expire_me");
    createRev(fresh, docID, kRevID, kFleeceBody);
    C4Timestamp expire = c4_now() + 1000;
    CHECK(!c4doc_setExpiration(db, docID, expire, &err));
    CHECK(err == notFound);
    REQUIRE(c4coll_setDocExpiration(fresh, docID, expire, WITH_ERROR()));


    expire = c4_now() + 2000;
    // Make sure setting it to the same is also true
    CHECK(!c4doc_setExpiration(db, docID, expire, &err));
    CHECK(err == notFound);
    REQUIRE(c4coll_setDocExpiration(fresh, docID, expire, WITH_ERROR()));
    REQUIRE(c4coll_setDocExpiration(fresh, docID, expire, WITH_ERROR()));

    C4Slice docID2 = C4STR("expire_me_too");
    createRev(fresh, docID2, kRevID, kFleeceBody);
    CHECK(!c4doc_setExpiration(db, docID2, expire, &err));
    CHECK(err == notFound);
    REQUIRE(c4coll_setDocExpiration(fresh, docID2, expire, WITH_ERROR()));

    C4Slice docID3 = C4STR("dont_expire_me");
    createRev(fresh, docID3, kRevID, kFleeceBody);

    C4Slice docID4 = C4STR("expire_me_later");
    createRev(fresh, docID4, kRevID, kFleeceBody);
    CHECK(!c4doc_setExpiration(db, docID4, expire + 100000, &err));
    CHECK(err == notFound);
    REQUIRE(c4coll_setDocExpiration(fresh, docID4, expire + 100000, WITH_ERROR()));

    REQUIRE(!c4coll_setDocExpiration(fresh, "nonexistent"_sl, expire + 50000, &err));
    CHECK(err == notFound);

    CHECK(c4coll_getDocExpiration(fresh, docID, nullptr) == expire);
    CHECK(c4coll_getDocExpiration(fresh, docID2, nullptr) == expire);
    CHECK(c4coll_getDocExpiration(fresh, docID3, nullptr) == C4Timestamp::None);
    CHECK(c4coll_getDocExpiration(fresh, docID4, nullptr) == expire + 100000);
    CHECK(c4coll_getDocExpiration(fresh, "nonexistent"_sl, nullptr) == C4Timestamp::None);

    CHECK(c4coll_nextDocExpiration(fresh) == expire);

    // Wait for the expiration time to pass:
    C4Log("---- Wait till expiration time...");
    this_thread::sleep_for(2500ms);
    REQUIRE(c4_now() >= expire);

    CHECK(!docExists(fresh, docID));
    CHECK(!docExists(fresh, docID2));
    CHECK(docExists(fresh, docID3));
    CHECK(docExists(fresh, docID4));

    CHECK(c4coll_nextDocExpiration(fresh) == expire + 100000);

    C4Log("---- Purge expired docs");
    CHECK(c4coll_purgeExpiredDocs(fresh, WITH_ERROR()) == 0);
}

N_WAY_TEST_CASE_METHOD(C4CollectionTest, "Move Doc between Collections", "[Database][Collection][C]") {
    // Create "guitars" collection:
    C4Collection* guitars = c4db_createCollection(db, Guitars, ERROR_INFO());
    REQUIRE(guitars);
    C4Collection* dflt = requireCollection(db);

    // Add a document to collection Guitars:
    {
        C4Database::Transaction t(db);
        addNumberedDocs(guitars, 1);
        t.commit();
    }

    CHECK(c4coll_getDocumentCount(guitars) == 1);
    CHECK(c4coll_getDocumentCount(dflt) == 0);

    bool succ = c4coll_moveDoc(guitars, "doc-001"_sl, dflt, "from-doc-001"_sl, ERROR_INFO());
    CHECK(succ);
    CHECK(c4coll_getDocumentCount(guitars) == 0);
    CHECK(c4coll_getDocumentCount(dflt) == 1);
    CHECK(docExists(dflt, "from-doc-001"_sl));
}
