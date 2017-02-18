//
//  c4DatabaseInternal.cc
//  Couchbase Lite Core
//
//  Created by hideki on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4Private.h"
#include "c4DocEnumerator.h"
#include "c4ExpiryEnumerator.h"
#include "c4BlobStore.h"
#include <cmath>
#include <errno.h>
#include <iostream>

#include "sqlite3.h"

#ifdef _MSC_VER
#include <ctime>
#include "Windows.h"
#define sleep(sec) Sleep((sec)*1000)
#else
#include "unistd.h"
#endif

// For debugging
#define C4STR_TO_STDSTR(x) std::string((char*)x.buf, x.size)
#define C4STR_TO_CSTR(x)   C4STR_TO_STDSTR(x).c_str()


////////////////////////////////////////////////////////////////////////////////
// Ported from DatabaseInternal_Tests.m
////////////////////////////////////////////////////////////////////////////////
class C4DatabaseInternalTest : public C4Test {
public:
    
    C4DatabaseInternalTest(int testOption) :C4Test(testOption) { }
    
    void assertMessage(C4ErrorDomain domain, int code, const char *expectedMsg) {
        C4SliceResult msg = c4error_getMessage({domain, code});
        REQUIRE(std::string((char*)msg.buf, msg.size) == std::string(expectedMsg));
        c4slice_free(msg);
        
        char buf[256];
        char *cmsg = c4error_getMessageC({domain, code}, buf, sizeof(buf));
        REQUIRE(std::string(cmsg) == std::string(expectedMsg));
        REQUIRE(cmsg == &buf[0]);
    }
    
    C4Document* putDoc(C4Slice docID, C4Slice revID, C4Slice body,
                       C4RevisionFlags flags = 0) {
        return putDoc(db, docID, revID, body, flags);
    }
    
    C4Document* putDoc(C4Database *db, C4Slice docID, C4Slice revID,
                       C4Slice body, C4RevisionFlags flags) {
        C4Error error = {};
        C4Document* doc = putDoc(db, docID, revID, body, flags, &error);
        REQUIRE(doc);
        REQUIRE(error.domain == 0);
        REQUIRE(error.code == 0);
        return doc;
    };
    
    C4Document* putDoc(C4Database *db, C4Slice docID, C4Slice revID,
                       C4Slice body, C4RevisionFlags flags, C4Error* error) {
        TransactionHelper t(db);
        C4Slice history[1] = {revID};
        C4DocPutRequest rq = {};
        rq.docID = docID;
        rq.history = revID == kC4SliceNull?NULL:history;
        rq.historyCount = revID == kC4SliceNull?0:1,
        rq.body = body;
        rq.revFlags = flags;
        rq.save = true;
        return c4doc_put(db, &rq, nullptr, error);
    }
    
    // create, update, delete doc must fail
    void putDocMustFail(C4Slice docID, C4Slice revID,
                        C4Slice body, C4RevisionFlags flags, C4Error expected) {
        putDocMustFail(db, docID, revID, body, flags, expected);
    }
    
    // create, update, delete doc must fail
    void putDocMustFail(C4Database *db, C4Slice docID, C4Slice revID,
                        C4Slice body, C4RevisionFlags flags, C4Error expected) {
        C4Error error = {};
        C4Document* doc = putDoc(db, docID, revID, body, flags, &error);
        REQUIRE(doc == nullptr);
        REQUIRE(error.domain == expected.domain);
        REQUIRE(error.code == expected.code);
    };
    
    C4String copy(C4String str){
        void *copied = ::malloc(str.size);
        if(copied)
            ::memcpy(copied, str.buf, str.size);
        return {copied, str.size};
    }
    
    void free(C4String str){
        if(str.buf)
            ::free((void*)str.buf);
    }
};

// test01_CRUD
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "CRUD", "[Database][C]") {
    if(isVersionVectors()) return;
    
    C4Error c4err;
    C4String body = C4STR("{\"foo\":1, \"bar\":false}");
    C4String updatedBody = C4STR("{\"foo\":1, \"bar\":false, \"status\":\"updated!\"}");
    
    // Make sure the database-changed notifications have the right data in them (see issue #93)
    
    // Get a nonexistent document:
    REQUIRE(c4doc_get(db, C4STR("nonexistent"), true, &c4err) == NULL);
    REQUIRE(c4err.domain == LiteCoreDomain);
    REQUIRE(c4err.code == kC4ErrorNotFound);
    
    // Create a document:
    // kRevKeepBody => Revision's body should not be discarded when non-leaf
    C4Document* doc = putDoc(kC4SliceNull, kC4SliceNull, body, kRevKeepBody);
    REQUIRE(doc->docID.size >= 10);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "1-") == 0);
    C4String docID = copy(doc->docID);
    C4String revID1 = copy(doc->revID);
    c4doc_free(doc);
    
    // Read it back:
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->selectedRev.revID == revID1);
    REQUIRE(doc->selectedRev.body == body);
    c4doc_free(doc);
    
    // Now update it:
    doc = putDoc(docID, revID1, updatedBody, kRevKeepBody);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->selectedRev.body == updatedBody);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "2-") == 0);
    C4String revID2 = copy(doc->revID);
    c4doc_free(doc);
    
    // Read it back:
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->selectedRev.revID == revID2);
    REQUIRE(doc->selectedRev.body == updatedBody);
    c4doc_free(doc);
    
    // Try to update the first rev, which should fail:
    putDocMustFail(docID, revID1, updatedBody, kRevKeepBody,
                   {.domain = LiteCoreDomain, .code = kC4ErrorConflict});
    
    // Check the changes feed, with and without filters:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4DocEnumerator* e = c4db_enumerateChanges(db, 0, &options, &c4err);
    REQUIRE(e);
    C4SequenceNumber seq = 2;
    while (nullptr != (doc = c4enum_nextDocument(e, &c4err))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        REQUIRE(doc->selectedRev.revID == revID2);
        REQUIRE(doc->docID == docID);
        c4doc_free(doc);
        seq++;
    }
    REQUIRE(seq == 3); // size check 2 + 0 => 3
    c4enum_free(e);
    
    // NOTE: Filter is out of LiteCore scope
    
    // Delete it:
    
    // without previous revision ID -> error
    putDocMustFail(docID, kC4SliceNull, kC4SliceNull, kRevDeleted,
                   {.domain = LiteCoreDomain, .code = kC4ErrorConflict});
    
    // with previous revision ID -> success
    doc = putDoc(docID, revID2, kC4SliceNull, kRevDeleted);
    REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kDeleted));
    REQUIRE(doc->docID == docID);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0);
    C4String revID3 = copy(doc->revID);
    c4doc_free(doc);
    
    // Read the deletion revision:
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->revID == revID3);
    REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kDeleted));
    REQUIRE(doc->selectedRev.revID == revID3);
    REQUIRE(doc->selectedRev.body == kC4SliceNull);
    REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)(kRevLeaf|kRevDeleted));
    c4doc_free(doc);
    
    // Delete nonexistent doc:
    putDocMustFail(C4STR("fake"), kC4SliceNull, kC4SliceNull, kRevDeleted,
                   {.domain = LiteCoreDomain, .code = kC4ErrorNotFound});
    
    // Read it back (should fail):
    // NOTE: LiteCore's c4doc_get() returns document even though document is deleted.
    //       Returning null doc is above layer's implementation.
    
    // Check the changes feed again after the deletion:
    // without deleted -> 0
    c4err = {};
    e = c4db_enumerateChanges(db, 0, &options, &c4err);
    REQUIRE(e);
    seq = 3;
    while (nullptr != (doc = c4enum_nextDocument(e, &c4err))) {
        c4doc_free(doc);
        seq++;
    }
    REQUIRE(seq == 3); // size check 3 + 0 => 4
    c4enum_free(e);
    
    // with deleted -> 1
    c4err = {};
    options.flags |= kC4IncludeDeleted;
    e = c4db_enumerateChanges(db, 0, &options, &c4err);
    REQUIRE(e);
    seq = 3;
    while (nullptr != (doc = c4enum_nextDocument(e, &c4err))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        REQUIRE(doc->selectedRev.revID == revID3);
        REQUIRE(doc->docID == docID);
        c4doc_free(doc);
        seq++;
    }
    REQUIRE(seq == 4); // size check 3 + 1 => 4
    c4enum_free(e);
    
    // Check the revision-history object (_revisions property):
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    int latest = 3;
    do{
        switch(latest){
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
    }while(c4doc_selectParentRevision(doc));
    REQUIRE(latest == 0);
    c4doc_free(doc);
    
    // NOTE: Following two methods are implemennted in above layer if necessary.
    // - (NSArray<CBL_RevID*>*) getRevisionHistory: (CBL_Revision*)rev
    //                                backToRevIDs: (NSArray<CBL_RevID*>*)ancestorRevIDs
    // + (NSDictionary*) makeRevisionHistoryDict: (NSArray<CBL_RevID*>*)history
    
    // Read rev 1 again:
    c4err = {};
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    c4err = {};
    c4doc_selectRevision(doc, revID1, true, &c4err);
    REQUIRE(doc->selectedRev.revID == revID1);
    REQUIRE(doc->selectedRev.body == body);
    c4doc_free(doc);
    
    // Compact the database:
    c4err = {};
    REQUIRE(c4db_compact(db, &c4err));
    
    // Make sure old rev is missing:
    c4err = {};
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    c4err = {};
    REQUIRE(c4doc_selectRevision(doc, revID1, true, &c4err));
    REQUIRE(doc->selectedRev.revID == revID1);
    // TODO: c4db_compact() implementation is still work in progress.
    //       Following line should be activated after implementation.
    // REQUIRE(doc->selectedRev.body == kC4SliceNull);
    REQUIRE(doc->selectedRev.body == body);
    c4doc_free(doc);
    
    // Make sure history still works after compaction:
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    latest = 3;
    do{
        switch(latest){
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
    }while(c4doc_selectParentRevision(doc));
    REQUIRE(latest == 0);
    c4doc_free(doc);
    
    // release allocated memories
    free(docID);
    free(revID1);
    free(revID2);
    free(revID3);
}


// test02_EmptyDoc
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "EmptyDoc", "[Database][C]") {
    // Test case for issue #44, which is caused by a bug in CBLJSON.
    
    if(isVersionVectors()) return;
    
    // Create a document:
    C4Document* doc = putDoc(kC4SliceNull, kC4SliceNull, C4STR("{}"));
    C4String docID = copy(doc->docID);
    c4doc_free(doc);
    
    C4Error error = {};
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4String keys[1] = {docID};
    C4DocEnumerator *e = c4db_enumerateSomeDocs(db, keys, 1, &options, &error);
    REQUIRE(e);
    C4SequenceNumber seq = 1;
    while (nullptr != (doc = c4enum_nextDocument(e, &error))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        REQUIRE(doc->docID == docID);
        c4doc_free(doc);
        seq++;
    }
    REQUIRE(seq == 2); // size check 1 + 1 => 2
    c4enum_free(e);
    
    // free copied C4String instances
    free(docID);
}

// test02_ExpectedRevIDs
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "ExpectedRevIDs", "[Database][C]") {
    // It's not strictly required that revisions always generate the same revIDs, but it helps
    // prevent false conflicts when two peers make the same change to the same parent revision.
    if(isVersionVectors()) return;
    
    // Create a document:
    C4Document* doc = putDoc(C4STR("doc"), kC4SliceNull, C4STR("{\"property\":\"value\"}"));
    REQUIRE(doc->revID == C4STR("1-3de83144ab0b66114ff350b20724e1fd48c6c57b"));
    C4String docID = copy(doc->docID);
    C4String revID1 = copy(doc->revID);
    c4doc_free(doc);
    
    // Update a document
    doc = putDoc(docID, revID1, C4STR("{\"property\":\"newvalue\"}"));
    REQUIRE(doc->revID == C4STR("2-7718b0324ed598dda05874ab0afa1c826a4dc45c"));
    C4String revID2 = copy(doc->revID);
    c4doc_free(doc);

    // Delete a document
    doc = putDoc(docID, revID2, kC4SliceNull, kRevDeleted);
    REQUIRE(doc->revID == C4STR("3-6f61ee6f47b9f70773aa769d97b116d615cad7b9"));
    c4doc_free(doc);
    
    // free copied C4String instances
    free(docID);
    free(revID1);
    free(revID2);
}

// test03_DeleteWithProperties
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "DeleteWithProperties", "[Database][C]") {
    // Test case for issue #50.
    // Test that it's possible to delete a document by PUTting a revision with _deleted=true,
    // and that the saved deleted revision will preserve any extra properties.
    
    if(isVersionVectors()) return;
    
    // Create a document:
    C4String body1 = C4STR("{\"property\":\"newvalue\"}");
    C4Document* doc = putDoc(kC4SliceNull, kC4SliceNull, body1);
    C4String docID = copy(doc->docID);
    C4String revID1 = copy(doc->revID);
    c4doc_free(doc);
    
    // Delete a document
    C4String body2 = C4STR("{\"property\":\"newvalue\"}");
    doc = putDoc(docID, revID1, body2, kRevDeleted);
    C4String revID2 = copy(doc->revID);
    c4doc_free(doc);
    
    // NOTE: LiteCore level c4doc_get() return non-null document, but
    //       higher level should return null.
    C4Error error = {};
    doc = c4doc_get(db, docID, true, &error);
    REQUIRE(doc);
    REQUIRE(c4doc_selectRevision(doc, revID2, true, &error));
    REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kDeleted));
    REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)(kRevLeaf|kRevDeleted));
    REQUIRE(doc->selectedRev.body == body2);
    c4doc_free(doc);
    
    // Make sure it's possible to create the doc from scratch again:
    doc = putDoc(docID, kC4SliceNull, body2);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0); // new rev is child of tombstone rev
    C4String revID3 = copy(doc->revID);
    c4doc_free(doc);
    
    doc = c4doc_get(db, docID, true, &error);
    REQUIRE(doc);
    REQUIRE(doc->revID == revID3);
    c4doc_free(doc);
    
    // free copied C4String instances
    free(docID);
    free(revID1);
    free(revID2);
    free(revID3);
}

// test04_DeleteAndRecreate
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "DeleteAndRecreate", "[Database][C]") {
    // Test case for issue #205: Create a doc, delete it, create it again with the same content.
    
    if(isVersionVectors()) return;
    
    // Create a document:
    C4String body = C4STR("{\"property\":\"value\"}");
    C4Document* doc = putDoc(C4STR("dock"), kC4SliceNull, body);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "1-") == 0);
    REQUIRE(doc->selectedRev.body == body);
    C4String revID1 = copy(doc->revID);
    c4doc_free(doc);
    
    // Delete a document
    doc = putDoc(C4STR("dock"), revID1, kC4SliceNull, kRevDeleted);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "2-") == 0);
    REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kDeleted));
    REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)(kRevLeaf|kRevDeleted));
    REQUIRE(doc->selectedRev.body == kC4SliceNull);
    C4String revID2 = copy(doc->revID);
    c4doc_free(doc);
    
    // Recreate a document with same content with revision 1
    doc = putDoc(C4STR("dock"), revID2, body);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0);
    REQUIRE(doc->selectedRev.body == body);
    c4doc_free(doc);
}

// test05_Validation
// test06_RevTree
// test07_RevTreeConflict
// test08_DeterministicRevIDs
// test09_DuplicateRev
// test16_ReplicatorSequences
// test17_LocalDocs
// test18_FindMissingRevisions
// test19_Purge
// test20_PurgeRevs
// test21_DeleteDatabase
// test22_Manager_Close
// test23_MakeRevisionHistoryDict
// test24_UpgradeDB
// test25_FileProtection
// test26_ReAddAfterPurge
// test27_ChangesSinceSequence
// test28_enableAutoCompact
// test29_autoPruneOnPut
// test29_autoPruneOnForceInsert
// test30_conflictAfterPrune
