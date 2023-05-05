//
// c4DatabaseInternal.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Test.hh"  // IWYU pragma: keep
#include "c4Collection.h"
#include "c4DocEnumerator.h"
#include "c4Document+Fleece.h"
#include <cmath>
#include <cerrno>
#include <iostream>
#include <vector>

#include "sqlite3.h"

#ifdef _MSC_VER
#    include <ctime>
#    include "Windows.h"
#    define sleep(sec) Sleep((sec)*1000)
#endif

// For debugging
#define C4STR_TO_STDSTR(x) std::string((char*)(x).buf, (x).size)
#define C4STR_TO_CSTR(x)   C4STR_TO_STDSTR(x).c_str()

static C4Document* c4enum_nextDocument(C4DocEnumerator* e, C4Error* outError) noexcept {
    return c4enum_next(e, outError) ? c4enum_getDocument(e, outError) : nullptr;
}

////////////////////////////////////////////////////////////////////////////////
// Ported from DatabaseInternal_Tests.m
////////////////////////////////////////////////////////////////////////////////
class C4DatabaseInternalTest : public C4Test {
  public:
    C4RemoteID _remoteID{0};

    explicit C4DatabaseInternalTest(int testOption) : C4Test(testOption) {}

    static void assertMessage(C4ErrorDomain domain, int code, const char* expectedMsg) {
        C4SliceResult msg = c4error_getMessage({domain, code});
        REQUIRE(std::string((char*)msg.buf, msg.size) == std::string(expectedMsg));
        c4slice_free(msg);

        char  buf[256];
        char* cmsg = c4error_getDescriptionC({domain, code}, buf, sizeof(buf));
        REQUIRE(std::string(cmsg) == std::string(expectedMsg));
        REQUIRE(cmsg == &buf[0]);
    }

    C4Document* getDoc(C4String docID, C4DocContentLevel content = kDocGetCurrentRev) {
        return getDoc(db, docID, content);
    }

    static C4Document* getDoc(C4Database* db, C4String docID, C4DocContentLevel content = kDocGetCurrentRev) {
        auto        defaultColl = c4db_getDefaultCollection(db, nullptr);
        C4Document* doc         = c4coll_getDoc(defaultColl, docID, true, content, ERROR_INFO());
        REQUIRE(doc);
        REQUIRE(doc->docID == docID);
        return doc;
    }

    C4Document* putDoc(C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags = 0) {
        return putDoc(db, docID, revID, body, flags);
    }

    C4Document* putDoc(C4Database* db, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags) {
        C4Document* doc = putDoc(db, docID, revID, body, flags, ERROR_INFO());
        REQUIRE(doc);
        return doc;
    }

    alloc_slice encodeBodyIfJSON(C4Slice body) {
        if ( slice(body).hasPrefix("{"_sl) && slice(body).hasSuffix("}"_sl) ) {
            // auto-convert JSON to Fleece as a convenience for the tests
            return json2fleece(std::string(slice(body)).c_str());
        } else {
            return alloc_slice(body);
        }
    }

    C4Document* putDoc(C4Database* db, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags,
                       C4Error* error) {
        TransactionHelper t(db);
        alloc_slice       encodedBody = encodeBodyIfJSON(body);
        C4Slice           history[1]  = {revID};
        C4DocPutRequest   rq          = {};
        rq.allowConflict              = false;
        rq.docID                      = docID;
        rq.history                    = history;
        rq.historyCount               = (size_t)(revID == kC4SliceNull ? 0 : 1);
        rq.body                       = encodedBody;
        rq.revFlags                   = flags;
        rq.save                       = true;
        rq.remoteDBID                 = _remoteID;
        auto defaultColl              = getCollection(db, kC4DefaultCollectionSpec);
        return c4coll_putDoc(defaultColl, &rq, nullptr, error);
    }

    void forceInsert(C4Slice docID, const C4Slice* history, size_t historyCount, C4Slice body,
                     C4RevisionFlags flags = 0) {
        C4Document* doc = forceInsert(db, docID, history, historyCount, body, flags);
        c4doc_release(doc);
    }

    C4Document* forceInsert(C4Database* db, C4Slice docID, const C4Slice* history, size_t historyCount, C4Slice body,
                            C4RevisionFlags flags) {
        C4Error     error = {};
        C4Document* doc   = forceInsert(db, docID, history, historyCount, body, flags, ERROR_INFO(error));
        REQUIRE(doc);
        return doc;
    }

    C4Document* forceInsert(C4Database* db, C4Slice docID, const C4Slice* history, size_t historyCount, C4Slice body,
                            C4RevisionFlags flags, C4Error* error) {
        TransactionHelper t(db);
        alloc_slice       encodedBody = encodeBodyIfJSON(body);
        C4DocPutRequest   rq          = {};
        rq.docID                      = docID;
        rq.existingRevision           = true;
        rq.allowConflict              = true;
        rq.history                    = history;
        rq.historyCount               = historyCount;
        rq.body                       = encodedBody;
        rq.revFlags                   = flags;
        rq.save                       = true;
        rq.remoteDBID                 = _remoteID;
        size_t commonAncestorIndex;
        auto   defaultColl = getCollection(db, kC4DefaultCollectionSpec);
        return c4coll_putDoc(defaultColl, &rq, &commonAncestorIndex, error);
    }

    // create, update, delete doc must fail
    void putDocMustFail(C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags, C4Error expected) {
        putDocMustFail(db, docID, revID, body, flags, expected);
    }

    // create, update, delete doc must fail
    void putDocMustFail(C4Database* db, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags,
                        C4Error expected) {
        ExpectingExceptions x;
        C4Error             error = {};
        C4Document*         doc   = putDoc(db, docID, revID, body, flags, &error);
        REQUIRE(doc == nullptr);
        REQUIRE(error.domain == expected.domain);
        REQUIRE(error.code == expected.code);
    };

    static std::vector<alloc_slice> getAllParentRevisions(C4Document* doc) {
        std::vector<alloc_slice> history;
        do { history.emplace_back(doc->selectedRev.revID); } while ( c4doc_selectParentRevision(doc) );
        return history;
    }

    static std::vector<alloc_slice> getRevisionHistory(C4Document* doc, bool onlyCurrent, bool includeDeleted) {
        std::vector<alloc_slice> history;
        do {
            if ( onlyCurrent && !(doc->selectedRev.flags & kRevLeaf) ) continue;
            if ( !includeDeleted && (doc->selectedRev.flags & kRevDeleted) ) continue;
            history.emplace_back(doc->selectedRev.revID);
        } while ( c4doc_selectNextRevision(doc) );
        return history;
    }

    void verifyRev(C4Document* doc, const C4String* history, int historyCount, C4String body) {
        REQUIRE(doc->revID == history[0]);
        REQUIRE(doc->selectedRev.revID == history[0]);
        REQUIRE(docBodyEquals(doc, body));

        std::vector<alloc_slice> revs = getAllParentRevisions(doc);
        REQUIRE(revs.size() == historyCount);
        for ( int i = 0; i < historyCount; i++ ) REQUIRE(history[i] == revs[i]);
    }
};

// test01_CRUD
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "CRUD", "[Database][C]") {
    if ( !isRevTrees() ) return;

    C4Error     c4err;
    alloc_slice body        = json2fleece("{'foo':1, 'bar':false}");
    alloc_slice updatedBody = json2fleece("{'foo':1, 'bar':false, 'status':'updated!'}");

    // Make sure the database-changed notifications have the right data in them (see issue #93)
    // TODO: Observer

    // Get a nonexistent document:
    REQUIRE(c4doc_get(db, C4STR("nonexistent"), true, &c4err) == NULL);
    REQUIRE(c4err.domain == LiteCoreDomain);
    REQUIRE(c4err.code == kC4ErrorNotFound);

    // Create a document:
    // kRevKeepBody => Revision's body should not be discarded when non-leaf
    C4Document* doc = putDoc(kC4SliceNull, kC4SliceNull, body, kRevKeepBody);
    REQUIRE(doc->docID.size >= 10);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "1-") == 0);
    alloc_slice docID  = doc->docID;
    alloc_slice revID1 = doc->revID;
    c4doc_release(doc);

    // Read it back:
    auto defaultColl = c4db_getDefaultCollection(db, nullptr);
    doc              = c4coll_getDoc(defaultColl, docID, true, kDocGetCurrentRev, ERROR_INFO(&c4err));
    REQUIRE(doc);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->selectedRev.revID == revID1);
    REQUIRE(docBodyEquals(doc, body));
    c4doc_release(doc);

    // Now update it:
    doc = putDoc(docID, revID1, updatedBody, kRevKeepBody);
    REQUIRE(doc->docID == docID);
    REQUIRE(docBodyEquals(doc, updatedBody));
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "2-") == 0);
    alloc_slice revID2 = doc->revID;
    c4doc_release(doc);

    // Read it back:
    doc = c4coll_getDoc(defaultColl, docID, true, kDocGetCurrentRev, ERROR_INFO(&c4err));
    REQUIRE(doc);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->selectedRev.revID == revID2);
    REQUIRE(docBodyEquals(doc, updatedBody));
    c4doc_release(doc);

    // Try to update the first rev, which should fail:
    C4Error err;
    err.domain = LiteCoreDomain;
    err.code   = kC4ErrorConflict;
    putDocMustFail(docID, revID1, updatedBody, kRevKeepBody, err);

    // Check the changes feed, with and without filters:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4DocEnumerator*    e       = c4coll_enumerateChanges(defaultColl, 0, &options, ERROR_INFO(&c4err));
    REQUIRE(e);
    C4SequenceNumber seq = 2;
    while ( nullptr != (doc = c4enum_nextDocument(e, ERROR_INFO(&c4err))) ) {
        REQUIRE(doc->selectedRev.sequence == seq);
        REQUIRE(doc->selectedRev.revID == revID2);
        REQUIRE(doc->docID == docID);
        c4doc_release(doc);
        seq++;
    }
    REQUIRE(seq == 3);  // size check 2 + 0 => 3
    c4enum_free(e);

    // NOTE: Filter is out of LiteCore scope

    // Delete it:

    // without previous revision ID -> error
    err.domain = LiteCoreDomain;
    err.code   = kC4ErrorInvalidParameter;
    putDocMustFail(docID, kC4SliceNull, kC4SliceNull, kRevDeleted, err);

    // with previous revision ID -> success
    doc = putDoc(docID, revID2, kC4SliceNull, kRevDeleted);
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    REQUIRE(doc->docID == docID);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0);
    alloc_slice revID3 = doc->revID;
    c4doc_release(doc);

    // Read the deletion revision:
    doc = c4coll_getDoc(defaultColl, docID, true, kDocGetCurrentRev, ERROR_INFO(&c4err));
    REQUIRE(doc);
    CHECK(doc->docID == docID);
    CHECK(doc->revID == revID3);
    CHECK(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    CHECK(doc->selectedRev.revID == revID3);
    CHECK(c4doc_getProperties(doc) != nullptr);  // valid revision should not have null body
    CHECK(doc->selectedRev.flags == (C4RevisionFlags)(kRevLeaf | kRevDeleted));
    c4doc_release(doc);

    // Delete nonexistent doc:
    putDocMustFail(C4STR("fake"), kC4SliceNull, kC4SliceNull, kRevDeleted, err);

    // Read it back (should fail):
    // NOTE: LiteCore's c4doc_get() returns document even though document is deleted.
    //       Returning null doc is above layer's implementation.

    // Check the changes feed again after the deletion:
    // without deleted -> 0
    e = c4coll_enumerateChanges(defaultColl, 0, &options, ERROR_INFO(&c4err));
    REQUIRE(e);
    seq = 3;
    while ( nullptr != (doc = c4enum_nextDocument(e, ERROR_INFO(&c4err))) ) {
        c4doc_release(doc);
        seq++;
    }
    REQUIRE(seq == 3);  // size check 3 + 0 => 4
    c4enum_free(e);

    // with deleted -> 1
    options.flags |= kC4IncludeDeleted;
    e = c4coll_enumerateChanges(defaultColl, 0, &options, ERROR_INFO(&c4err));
    REQUIRE(e);
    seq = 3;
    while ( nullptr != (doc = c4enum_nextDocument(e, ERROR_INFO(&c4err))) ) {
        REQUIRE(doc->selectedRev.sequence == seq);
        REQUIRE(doc->selectedRev.revID == revID3);
        REQUIRE(doc->docID == docID);
        c4doc_release(doc);
        seq++;
    }
    REQUIRE(seq == 4);  // size check 3 + 1 => 4
    c4enum_free(e);

    // Check the revision-history object (_revisions property):
    doc = c4coll_getDoc(defaultColl, docID, true, kDocGetAll, ERROR_INFO(&c4err));
    REQUIRE(doc);
    int latest = 3;
    do {
        switch ( latest ) {
            case 3:
                REQUIRE(doc->selectedRev.revID == revID3);
                break;
            case 2:
                REQUIRE(doc->selectedRev.revID == revID2);
                break;
            case 1:
                REQUIRE(doc->selectedRev.revID == revID1);
                break;
            default:
                // should not come here...
                REQUIRE(false);
        }
        latest--;
    } while ( c4doc_selectParentRevision(doc) );
    REQUIRE(latest == 0);
    c4doc_release(doc);

    // NOTE: Following two methods are implemennted in above layer if necessary.
    // - (NSArray<CBL_RevID*>*) getRevisionHistory: (CBL_Revision*)rev
    //                                backToRevIDs: (NSArray<CBL_RevID*>*)ancestorRevIDs
    // + (NSDictionary*) makeRevisionHistoryDict: (NSArray<CBL_RevID*>*)history

    // Read rev 2 again:
    doc = c4coll_getDoc(defaultColl, docID, true, kDocGetCurrentRev, ERROR_INFO(&c4err));
    REQUIRE(doc);
    CHECK(c4doc_selectRevision(doc, revID2, true, WITH_ERROR(&c4err)));
    REQUIRE(doc->selectedRev.revID == revID2);
    REQUIRE(docBodyEquals(doc, updatedBody));
    c4doc_release(doc);

    // Compact the database:
    REQUIRE(c4db_maintenance(db, kC4Compact, WITH_ERROR(&c4err)));

    // Make sure old rev is missing:
    doc = c4coll_getDoc(defaultColl, docID, true, kDocGetCurrentRev, ERROR_INFO(&c4err));
    REQUIRE(doc);
    REQUIRE(c4doc_selectRevision(doc, revID2, true, WITH_ERROR(&c4err)));
    REQUIRE(doc->selectedRev.revID == revID2);
    // TODO: c4db_compact() implementation is still work in progress.
    //       Following line should be activated after implementation.
    // REQUIRE(doc->selectedRev.body == kC4SliceNull);
    c4doc_release(doc);

    // Make sure history still works after compaction:
    doc = c4coll_getDoc(defaultColl, docID, true, kDocGetAll, ERROR_INFO(&c4err));
    REQUIRE(doc);
    latest = 3;
    do {
        switch ( latest ) {
            case 3:
                REQUIRE(doc->selectedRev.revID == revID3);
                break;
            case 2:
                REQUIRE(doc->selectedRev.revID == revID2);
                break;
            case 1:
                REQUIRE(doc->selectedRev.revID == revID1);
                break;
            default:
                // should not come here...
                REQUIRE(false);
        }
        latest--;
    } while ( c4doc_selectParentRevision(doc) );
    REQUIRE(latest == 0);
    c4doc_release(doc);
}

// test02_EmptyDoc
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "EmptyDoc", "[Database][C]") {
    // Test case for issue #44, which is caused by a bug in CBLJSON.

    if ( !isRevTrees() ) return;
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    // Create a document:
    C4Document* doc   = putDoc(kC4SliceNull, kC4SliceNull, kEmptyFleeceBody);
    alloc_slice docID = doc->docID;
    c4doc_release(doc);

    C4Error             error   = {};
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4DocEnumerator*    e       = c4coll_enumerateAllDocs(defaultColl, &options, ERROR_INFO(error));
    REQUIRE(e);
    C4SequenceNumber seq = 1;
    while ( nullptr != (doc = c4enum_nextDocument(e, ERROR_INFO(error))) ) {
        REQUIRE(doc->selectedRev.sequence == seq);
        REQUIRE(doc->docID == docID);
        c4doc_release(doc);
        seq++;
    }
    REQUIRE(seq == 2);  // size check 1 + 1 => 2
    c4enum_free(e);
}

// test02_ExpectedRevIDs
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "ExpectedRevIDs", "[Database][C]") {
    // It's not strictly required that revisions always generate the same revIDs, but it helps
    // prevent false conflicts when two peers make the same change to the same parent revision.
    if ( !isRevTrees() ) return;

    // Create a document:
    C4Document* doc   = putDoc(C4STR("doc"), kC4SliceNull, C4STR("{'property':'value'}"));
    C4Slice     revID = C4STR("1-d65a07abdb5c012a1bd37e11eef1d0aca3fa2a90");
    REQUIRE(doc->revID == revID);
    alloc_slice docID  = doc->docID;
    alloc_slice revID1 = doc->revID;
    c4doc_release(doc);

    // Update a document
    doc   = putDoc(docID, revID1, C4STR("{'property':'newvalue'}"));
    revID = C4STR("2-eaaa643f551df08eb0c60f87f3f011ac4355f834");
    REQUIRE(doc->revID == revID);
    alloc_slice revID2 = doc->revID;
    c4doc_release(doc);

    // Delete a document
    doc   = putDoc(docID, revID2, kC4SliceNull, kRevDeleted);
    revID = C4STR("3-3ae8fab29af3a5bfbfa5a4c5fd91c58214cb0c5a");
    REQUIRE(doc->revID == revID);
    c4doc_release(doc);
}

// test03_DeleteWithProperties
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "DeleteWithProperties", "[Database][C]") {
    // Test case for issue #50.
    // Test that it's possible to delete a document by PUTting a revision with _deleted=true,
    // and that the saved deleted revision will preserve any extra properties.

    if ( !isRevTrees() ) return;

    // Create a document:
    C4String    body1  = C4STR("{'property':'newvalue'}");
    C4Document* doc    = putDoc(kC4SliceNull, kC4SliceNull, body1);
    alloc_slice docID  = doc->docID;
    alloc_slice revID1 = doc->revID;
    c4doc_release(doc);

    // Delete a document
    alloc_slice body2  = json2fleece("{'property':'newvalue'}");
    doc                = putDoc(docID, revID1, body2, kRevDeleted);
    alloc_slice revID2 = doc->revID;
    c4doc_release(doc);

    // NOTE: LiteCore level c4doc_get() return non-null document, but
    //       higher level should return null.
    C4Error error       = {};
    auto    defaultColl = c4db_getDefaultCollection(db, nullptr);
    doc                 = c4coll_getDoc(defaultColl, docID, true, kDocGetCurrentRev, ERROR_INFO(error));
    REQUIRE(doc);
    REQUIRE(c4doc_selectRevision(doc, revID2, true, WITH_ERROR(error)));
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)(kRevLeaf | kRevDeleted));
    REQUIRE(docBodyEquals(doc, body2));
    c4doc_release(doc);

    // Make sure it's possible to create the doc from scratch again:
    doc = putDoc(docID, kC4SliceNull, body2);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0);  // new rev is child of tombstone rev
    alloc_slice revID3 = doc->revID;
    c4doc_release(doc);

    doc = c4coll_getDoc(defaultColl, docID, true, kDocGetCurrentRev, ERROR_INFO(error));
    REQUIRE(doc);
    REQUIRE(doc->revID == revID3);
    c4doc_release(doc);
}

// test04_DeleteAndRecreate
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "DeleteAndRecreate", "[Database][C]") {
    // Test case for issue #205: Create a doc, delete it, create it again with the same content.

    if ( !isRevTrees() ) return;

    // Create a document:
    alloc_slice body = encodeBodyIfJSON("{'property':'value'}"_sl);
    C4Document* doc  = putDoc(C4STR("dock"), kC4SliceNull, body);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "1-") == 0);
    alloc_slice revID1 = doc->revID;
    c4doc_release(doc);

    // Delete a document
    doc = putDoc(C4STR("dock"), revID1, kC4SliceNull, kRevDeleted);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "2-") == 0);
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)(kRevLeaf | kRevDeleted));
    REQUIRE(c4doc_getProperties(doc) != nullptr);  // valid revision should not have null body
    alloc_slice revID2 = doc->revID;
    c4doc_release(doc);

    // Recreate a document with same content with revision 1
    doc = putDoc(C4STR("dock"), revID2, "{'property':'value'}"_sl);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0);
    REQUIRE(docBodyEquals(doc, body));
    c4doc_release(doc);
}

// test05_Validation
// NOTE: Validation should be done outside of LiteCore.

// test06_RevTree
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "RevTree", "[Database][C]") {
    if ( !isRevTrees() ) return;

    // TODO: Observer

    C4String       docID                 = C4STR("MyDocID");
    alloc_slice    body                  = json2fleece("{'message':'hi'}");
    const size_t   historyCount          = 4;
    const C4String history[historyCount] = {C4STR("4-4444"), C4STR("3-3333"), C4STR("2-2222"), C4STR("1-1111")};
    forceInsert(docID, history, historyCount, body);

    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    REQUIRE(c4coll_getDocumentCount(defaultColl) == 1);

    C4Document* doc = getDoc(docID, kDocGetAll);
    verifyRev(doc, history, historyCount, body);
    c4doc_release(doc);

    // No-op forceInsert: of already-existing revision:
    C4SequenceNumber lastSeq = c4db_getLastSequence(db);
    forceInsert(docID, history, historyCount, body);
    REQUIRE(c4db_getLastSequence(db) == lastSeq);

    // Insert a conflict:
    _remoteID                           = 1;  // Treat insertions as coming from a remote db by the replicator
    const size_t   conflictHistoryCount = 5;
    const C4String conflictHistory[conflictHistoryCount] = {C4STR("5-5555"), C4STR("4-4545"), C4STR("3-3030"),
                                                            C4STR("2-2222"), C4STR("1-1111")};
    alloc_slice    conflictBody                          = json2fleece("{'message':'yo'}");
    forceInsert(docID, conflictHistory, conflictHistoryCount, conflictBody);
    _remoteID = 0;

    // We handle conflicts somewhat differently now than in CBL 1. When a conflict is created the
    // new revision(s) are marked with kRevConflict, and such revisions can never be current. So
    // in other words the oldest revision always wins the conflict; it has nothing to do with the
    // revIDs.
    REQUIRE(c4coll_getDocumentCount(defaultColl) == 1);
    doc = getDoc(docID, kDocGetAll);
    verifyRev(doc, history, historyCount, body);
    c4doc_release(doc);
    //TODO - conflict check

    // Add an unrelated document:
    C4String       otherDocID                      = C4STR("AnotherDocID");
    alloc_slice    otherBody                       = json2fleece("{'language':'jp'}");
    const size_t   otherHistoryCount               = 1;
    const C4String otherHistory[otherHistoryCount] = {C4STR("1-1010")};
    forceInsert(otherDocID, otherHistory, otherHistoryCount, otherBody);

    // Fetch one of those phantom revisions with no body:
    doc           = getDoc(docID, kDocGetAll);
    C4Error error = {};
    REQUIRE(c4doc_selectRevision(doc, C4STR("2-2222"), false, WITH_ERROR(&error)));
    REQUIRE_FALSE((doc->selectedRev.flags & (C4RevisionFlags)(kRevKeepBody)));
    REQUIRE(c4doc_getProperties(doc) == nullptr);
    c4doc_release(doc);

    doc = getDoc(otherDocID, kDocGetAll);
    REQUIRE_FALSE(c4doc_selectRevision(doc, C4STR("666-6666"), false, &error));
    REQUIRE(error.domain == LiteCoreDomain);
    REQUIRE(error.code == kC4ErrorNotFound);
    c4doc_release(doc);

    // Make sure no duplicate rows were inserted for the common revisions:
    // LiteCore does note assigns sequence to inserted ancestor revs
    REQUIRE(c4db_getLastSequence(db) == 3);

    // Make sure the earlier revision wins the conflict:
    doc = getDoc(docID);
    REQUIRE(doc->revID == history[0]);
    REQUIRE(doc->selectedRev.revID == history[0]);
    c4doc_release(doc);

    // Check that the list of conflicts is accurate:
    doc                                      = getDoc(docID, kDocGetAll);
    std::vector<alloc_slice> conflictingRevs = getRevisionHistory(doc, true, true);
    REQUIRE(conflictingRevs.size() == 2);
    REQUIRE(conflictingRevs[0] == history[0]);
    REQUIRE(conflictingRevs[1] == conflictHistory[0]);
    c4doc_release(doc);

    // Get the _changes feed and verify only the winner is in it:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4DocEnumerator*    e       = c4coll_enumerateChanges(defaultColl, 0, &options, ERROR_INFO(error));
    REQUIRE(e);
    int counter = 0;
    while ( c4enum_next(e, ERROR_INFO(error)) ) {
        C4DocumentInfo docInfo;
        c4enum_getDocumentInfo(e, &docInfo);
        if ( counter == 0 ) {
            REQUIRE(docInfo.docID == docID);
            REQUIRE(docInfo.revID == history[0]);
        } else if ( counter == 1 ) {
            REQUIRE(docInfo.docID == otherDocID);
            REQUIRE(docInfo.revID == otherHistory[0]);
        }
        counter++;
    }
    c4enum_free(e);
    REQUIRE(counter == 2);

    options = kC4DefaultEnumeratorOptions;
    options.flags |= kC4IncludeDeleted;
    e = c4coll_enumerateChanges(defaultColl, 0, &options, ERROR_INFO(error));
    REQUIRE(e);
    counter = 0;
    while ( c4enum_next(e, ERROR_INFO(error)) ) {
        doc = c4enum_getDocument(e, ERROR_INFO(error));
        if ( !doc ) break;
        do {
            // NOTE: @[conflict, rev, other]
            if ( counter == 0 ) {
                REQUIRE(doc->docID == docID);
                REQUIRE(doc->selectedRev.revID == history[0]);
                REQUIRE(docBodyEquals(doc, body));
            } else if ( counter == 1 ) {
                REQUIRE(doc->docID == docID);
                REQUIRE(doc->selectedRev.revID == conflictHistory[0]);
                REQUIRE(docBodyEquals(doc, conflictBody));
            } else if ( counter == 2 ) {
                REQUIRE(doc->docID == otherDocID);
                REQUIRE(doc->selectedRev.revID == otherHistory[0]);
                REQUIRE(docBodyEquals(doc, otherBody));
            }
            counter++;
        } while ( c4doc_selectNextLeafRevision(doc, true, true, &error) );
        c4doc_release(doc);
    }
    c4enum_free(e);
    REQUIRE(counter == 3);


    // Verify that compaction leaves the document history:
    // TODO: compact() is not fully implemented
    //    error = {};
    //    REQUIRE(c4db_compact(db, WITH_ERROR(ERROR_INFO(error))));

    // Delete the current winning rev, leaving the other one:
    doc = putDoc(docID, conflictHistory[0], kC4SliceNull, kRevDeleted);
    c4doc_release(doc);
    doc = getDoc(docID);
    //TODO: Uncomment once https://github.com/couchbase/couchbase-lite-core/issues/57 is fixed
    //REQUIRE(doc->revID == history[0]); // 4-4444 should be current??
    //REQUIRE(doc->selectedRev.revID == history[0]);
    //verifyRev(doc, history, historyCount, body);
    c4doc_release(doc);

    // Delete the remaining rev:
    doc = putDoc(docID, history[0], kC4SliceNull, kRevDeleted);
    c4doc_release(doc);
    // TODO: Need to implement following tests
}

// test07_RevTreeConflict
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "RevTreeConflict", "[Database][C]") {
    if ( !isRevTrees() ) return;

    // Track the latest database-change notification that's posted:

    // TODO: Observer

    C4String       docID                 = C4STR("MyDocID");
    alloc_slice    body                  = json2fleece("{'message':'hi'}");
    const size_t   historyCount          = 1;
    const C4String history[historyCount] = {C4STR("1-1111")};
    C4Document*    doc                   = forceInsert(db, docID, history, historyCount, body, 0);
    auto           defaultColl           = getCollection(db, kC4DefaultCollectionSpec);
    REQUIRE(c4coll_getDocumentCount(defaultColl) == 1);
    verifyRev(doc, history, historyCount, body);
    c4doc_release(doc);

    const size_t   newHistoryCount             = 3;
    const C4String newHistory[newHistoryCount] = {C4STR("3-3333"), C4STR("2-2222"), C4STR("1-1111")};
    doc                                        = forceInsert(db, docID, newHistory, newHistoryCount, body, 0);
    REQUIRE(c4coll_getDocumentCount(defaultColl) == 1);
    verifyRev(doc, newHistory, newHistoryCount, body);
    c4doc_release(doc);
}

// test08_DeterministicRevIDs
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "DeterministicRevIDs", "[Database][C]") {
    if ( !isRevTrees() ) return;

    C4String    docID = C4STR("mydoc");
    C4String    body  = C4STR("{'key':'value'}");
    C4Document* doc   = putDoc(docID, kC4SliceNull, body);
    alloc_slice revID = doc->revID;
    c4doc_release(doc);

    deleteAndRecreateDB();

    doc = putDoc(docID, kC4SliceNull, body);
    REQUIRE(doc->revID == revID);
    REQUIRE(doc->selectedRev.revID == revID);
    c4doc_release(doc);
}

// test09_DuplicateRev
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "DuplicateRev", "[Database][C]") {
    if ( !isRevTrees() ) return;

    // rev1
    C4String    docID = C4STR("mydoc");
    alloc_slice body  = json2fleece("{'key':'value'}");
    C4Document* doc   = putDoc(docID, kC4SliceNull, body);
    alloc_slice revID = doc->revID;
    c4doc_release(doc);

    // rev2a
    body                = json2fleece("{'key':'new-value'}");
    doc                 = putDoc(docID, revID, body);
    alloc_slice revID2a = doc->revID;
    c4doc_release(doc);

    // rev2b
    {
        TransactionHelper t(db);
        C4Slice           history[1] = {revID};
        C4DocPutRequest   rq         = {};
        rq.allowConflict             = true;
        rq.docID                     = docID;
        rq.history                   = history;
        rq.historyCount              = 1;
        rq.body                      = body;
        rq.revFlags                  = 0;
        rq.save                      = true;
        C4Error error                = {};
        auto    defaultColl          = getCollection(db, kC4DefaultCollectionSpec);
        doc                          = c4coll_putDoc(defaultColl, &rq, nullptr, ERROR_INFO(error));
        REQUIRE(doc->docID == docID);
    }
    alloc_slice revID2b = doc->revID;
    c4doc_release(doc);

    REQUIRE(revID2a == revID2b);
}

#pragma mark - MISC.:

// test18_FindMissingRevisions
// test23_MakeRevisionHistoryDict
// test25_FileProtection
// test27_ChangesSinceSequence
// test29_autoPruneOnPut
// test29_autoPruneOnForceInsert
// test30_conflictAfterPrune
