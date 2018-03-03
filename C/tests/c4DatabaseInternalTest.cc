//
// c4DatabaseInternal.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "c4Test.hh"
#include "c4Private.h"
#include "c4DocEnumerator.h"
#include "c4ExpiryEnumerator.h"
#include "c4BlobStore.h"
#include <cmath>
#include <errno.h>
#include <iostream>
#include <vector>

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

static char sErrorMessageBuffer[256];
#define errorInfo(error) \
    INFO("Error: " << c4error_getMessageC(error, sErrorMessageBuffer, sizeof(sErrorMessageBuffer)))

static C4Document* c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError) noexcept {
    return c4enum_next(e, outError) ? c4enum_getDocument(e, outError) : nullptr;
}

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
    
    C4Document* getDoc(C4String docID){
        return getDoc(db, docID);
    }
    
    C4Document* getDoc(C4Database* db, C4String docID){
        C4Error error = {};
        C4Document* doc = c4doc_get(db, docID, true, &error);
        errorInfo(error);
        REQUIRE(doc);
        REQUIRE(doc->docID == docID);
        REQUIRE(error.domain == 0);
        REQUIRE(error.code == 0);
        return doc;
    }
    
    C4Document* putDoc(C4Slice docID, C4Slice revID,
                       C4Slice body, C4RevisionFlags flags = 0) {
        return putDoc(db, docID, revID, body, flags);
    }
    
    C4Document* putDoc(C4Database *db, C4Slice docID, C4Slice revID,
                       C4Slice body, C4RevisionFlags flags) {
        C4Error error = {};
        C4Document* doc = putDoc(db, docID, revID, body, flags, &error);
        errorInfo(error);
        REQUIRE(doc);
        return doc;
    }
    
    C4Document* putDoc(C4Database *db, C4Slice docID, C4Slice revID,
                       C4Slice body, C4RevisionFlags flags, C4Error* error) {
        TransactionHelper t(db);
        C4Slice history[1] = {revID};
        C4DocPutRequest rq = {};
        rq.allowConflict = false;
        rq.docID = docID;
        rq.history = revID == kC4SliceNull? NULL : history;
        rq.historyCount = (size_t)(revID == kC4SliceNull? 0 : 1);
        rq.body = body;
        rq.revFlags = flags;
        rq.save = true;
        return c4doc_put(db, &rq, nullptr, error);
    }
    
    void forceInsert(C4Slice docID, const C4Slice* history, size_t historyCount,
                            C4Slice body, C4RevisionFlags flags = 0) {
        C4Document* doc = forceInsert(db, docID, history, historyCount, body, flags);
        c4doc_free(doc);
    }
    
    C4Document* forceInsert(C4Database *db, C4Slice docID,
                            const C4Slice* history, size_t historyCount,
                            C4Slice body, C4RevisionFlags flags) {
        C4Error error = {};
        C4Document* doc = forceInsert(db, docID, history, historyCount,
                                      body, flags, &error);
        errorInfo(error);
        REQUIRE(doc);
        REQUIRE(error.domain == 0);
        REQUIRE(error.code == 0);
        return doc;
    }
    
    C4Document* forceInsert(C4Database *db, C4Slice docID,
                            const C4Slice* history, size_t historyCount,
                            C4Slice body, C4RevisionFlags flags,
                            C4Error* error) {
        TransactionHelper t(db);
        C4DocPutRequest rq = {};
        rq.docID = docID;
        rq.existingRevision = true;
        rq.allowConflict = true;
        rq.history = history;
        rq.historyCount = historyCount;
        rq.body = body;
        rq.revFlags = flags;
        rq.save = true;
        size_t commonAncestorIndex;
        return c4doc_put(db, &rq, &commonAncestorIndex, error);
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
        errorInfo(error);
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
    
    std::vector<C4String> getAllParentRevisions(C4Document* doc){
        std::vector<C4String> history;
        do{
            history.push_back(copy(doc->selectedRev.revID));
        }while(c4doc_selectParentRevision(doc));
        return history;
    }
    
    std::vector<C4String> getRevisionHistory(C4Document* doc, bool onlyCurrent, bool includeDeleted){
        std::vector<C4String> history;
        do{
            if(onlyCurrent && !(doc->selectedRev.flags & kRevLeaf))
                continue;
            if(!includeDeleted && (doc->selectedRev.flags & kRevDeleted))
                continue;
            history.push_back(copy(doc->selectedRev.revID));
        }while(c4doc_selectNextRevision(doc));
        return history;
    }
    
    void free(std::vector<C4String> revs){
        for(int i = 0; i < revs.size(); i++)
            free(revs.at(i));
    }
    
    void verifyRev(C4Document* doc, const C4String* history, int historyCount, C4String body){
        REQUIRE(doc->revID == history[0]);
        REQUIRE(doc->selectedRev.revID == history[0]);
        REQUIRE(doc->selectedRev.body == body);
        
        std::vector<C4String> revs = getAllParentRevisions(doc);
        REQUIRE(revs.size() == historyCount);
        for(int i = 0; i < historyCount; i++)
            REQUIRE(history[i] == revs[i]);
        free(revs);
    }
};

// test01_CRUD
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "CRUD", "[Database][C]") {
    if(!isRevTrees()) return;
    
    C4Error c4err;
    C4String body = C4STR("{\"foo\":1, \"bar\":false}");
    C4String updatedBody = C4STR("{\"foo\":1, \"bar\":false, \"status\":\"updated!\"}");
    
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
    C4Error err;
    err.domain = LiteCoreDomain;
    err.code = kC4ErrorConflict;
    putDocMustFail(docID, revID1, updatedBody, kRevKeepBody, err);
    
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
    err.domain = LiteCoreDomain;
    err.code = kC4ErrorInvalidParameter;
    putDocMustFail(docID, kC4SliceNull, kC4SliceNull, kRevDeleted, err);
    
    // with previous revision ID -> success
    doc = putDoc(docID, revID2, kC4SliceNull, kRevDeleted);
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    REQUIRE(doc->docID == docID);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0);
    C4String revID3 = copy(doc->revID);
    c4doc_free(doc);
    
    // Read the deletion revision:
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->revID == revID3);
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    REQUIRE(doc->selectedRev.revID == revID3);
    REQUIRE(doc->selectedRev.body != kC4SliceNull);  // valid revision should not have null body
    REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)(kRevLeaf|kRevDeleted));
    c4doc_free(doc);
    
    // Delete nonexistent doc:
    putDocMustFail(C4STR("fake"), kC4SliceNull, kC4SliceNull, kRevDeleted, err);
    
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
    
    // Read rev 2 again:
    c4err = {};
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    c4err = {};
    CHECK(c4doc_selectRevision(doc, revID2, true, &c4err));
    REQUIRE(doc->selectedRev.revID == revID2);
    REQUIRE(doc->selectedRev.body == updatedBody);
    c4doc_free(doc);
    
    // Compact the database:
    c4err = {};
    REQUIRE(c4db_compact(db, &c4err));
    
    // Make sure old rev is missing:
    c4err = {};
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    c4err = {};
    REQUIRE(c4doc_selectRevision(doc, revID2, true, &c4err));
    REQUIRE(doc->selectedRev.revID == revID2);
    // TODO: c4db_compact() implementation is still work in progress.
    //       Following line should be activated after implementation.
    // REQUIRE(doc->selectedRev.body == kC4SliceNull);
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
    
    if(!isRevTrees()) return;
    
    // Create a document:
    C4Document* doc = putDoc(kC4SliceNull, kC4SliceNull, C4STR("{}"));
    C4String docID = copy(doc->docID);
    c4doc_free(doc);
    
    C4Error error = {};
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4DocEnumerator *e = c4db_enumerateAllDocs(db, &options, &error);
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
    if(!isRevTrees()) return;
    
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
    
    if(!isRevTrees()) return;
    
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
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
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
    
    if(!isRevTrees()) return;
    
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
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)(kRevLeaf|kRevDeleted));
    REQUIRE(doc->selectedRev.body != kC4SliceNull);  // valid revision should not have null body
    C4String revID2 = copy(doc->revID);
    c4doc_free(doc);
    
    // Recreate a document with same content with revision 1
    doc = putDoc(C4STR("dock"), revID2, body);
    REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0);
    REQUIRE(doc->selectedRev.body == body);
    c4doc_free(doc);
}

// test05_Validation
// NOTE: Validation should be done outside of LiteCore.

// test06_RevTree
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "RevTree", "[Database][C]") {
    if(!isRevTrees()) return;
    
    // TODO: Observer
    
    C4String docID = C4STR("MyDocID");
    C4String body = C4STR("{\"message\":\"hi\"}");
    const size_t historyCount = 4;
    const C4String history[historyCount] =
        {C4STR("4-4444"), C4STR("3-3333"), C4STR("2-2222"), C4STR("1-1111")};
    forceInsert(docID, history, historyCount, body);

    REQUIRE(c4db_getDocumentCount(db) == 1);
    
    C4Document* doc = getDoc(docID);
    verifyRev(doc, history, historyCount, body);
    c4doc_free(doc);

    // No-op forceInsert: of already-existing revision:
    C4SequenceNumber lastSeq = c4db_getLastSequence(db);
    forceInsert(docID, history, historyCount, body);
    REQUIRE(c4db_getLastSequence(db) == lastSeq);
    
    const size_t conflictHistoryCount = 5;
    const C4String conflictHistory[conflictHistoryCount] =
    {C4STR("5-5555"), C4STR("4-4545"), C4STR("3-3030"),
        C4STR("2-2222"), C4STR("1-1111")};
    C4String conflictBody = C4STR("{\"message\":\"yo\"}");
    forceInsert(docID, conflictHistory, conflictHistoryCount, conflictBody);

    // We handle conflicts somewhat differently now than in CBL 1. When a conflict is created the
    // new revision(s) are marked with kRevConflict, and such revisions can never be current. So
    // in other words the oldest revision always wins the conflict; it has nothing to do with the
    // revIDs.
    REQUIRE(c4db_getDocumentCount(db) == 1);
    doc = getDoc(docID);
    verifyRev(doc, history, historyCount, body);
    c4doc_free(doc);
    //TODO - conflict check
    
    // Add an unrelated document:
    C4String otherDocID = C4STR("AnotherDocID");
    C4String otherBody = C4STR("{\"language\":\"jp\"}");
    const size_t otherHistoryCount = 1;
    const C4String otherHistory[otherHistoryCount] = {C4STR("1-1010")};
    forceInsert(otherDocID, otherHistory, otherHistoryCount, otherBody);

    // Fetch one of those phantom revisions with no body:
    doc = getDoc(docID);
    C4Error error = {};
    REQUIRE(c4doc_selectRevision(doc, C4STR("2-2222"), false, &error));
    REQUIRE((doc->selectedRev.flags & (C4RevisionFlags)(kRevKeepBody)) == false);
    REQUIRE(doc->selectedRev.body == kC4SliceNull);
    c4doc_free(doc);

    doc = getDoc(otherDocID);
    REQUIRE(c4doc_selectRevision(doc, C4STR("666-6666"), false, &error) == false);
    REQUIRE(error.domain == LiteCoreDomain);
    REQUIRE(error.code == kC4ErrorNotFound);
    c4doc_free(doc);
    
    // Make sure no duplicate rows were inserted for the common revisions:
    // LiteCore does note assigns sequence to inserted ancestor revs
    REQUIRE(c4db_getLastSequence(db) == 3);
    
    // Make sure the earlier revision wins the conflict:
    doc = getDoc(docID);
    REQUIRE(doc->revID == history[0]);
    REQUIRE(doc->selectedRev.revID == history[0]);
    c4doc_free(doc);
    
    // Check that the list of conflicts is accurate:
    doc = getDoc(docID);
    std::vector<C4String> conflictingRevs = getRevisionHistory(doc, true, true);
    REQUIRE(conflictingRevs.size() == 2);
    REQUIRE(conflictingRevs[0] == history[0]);
    REQUIRE(conflictingRevs[1] == conflictHistory[0]);
    c4doc_free(doc);
    
    // Get the _changes feed and verify only the winner is in it:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4DocEnumerator* e = c4db_enumerateChanges(db, 0, &options, &error);
    REQUIRE(e);
    int counter = 0;
    while(c4enum_next(e, &error)){
        C4DocumentInfo docInfo;
        c4enum_getDocumentInfo(e, &docInfo);
        if(counter == 0){
            REQUIRE(docInfo.docID == docID);
            REQUIRE(docInfo.revID == history[0]);
        }else if(counter == 1){
            REQUIRE(docInfo.docID == otherDocID);
            REQUIRE(docInfo.revID == otherHistory[0]);
        }
        counter++;
    }
    c4enum_free(e);
    REQUIRE(counter == 2);
    
    options = kC4DefaultEnumeratorOptions;
    options.flags |= kC4IncludeDeleted;
    e = c4db_enumerateChanges(db, 0, &options, &error);
    REQUIRE(e);
    counter = 0;
    while (c4enum_next(e, &error)) {
        doc = c4enum_getDocument(e, &error);
        if (!doc)
            break;
        do{
            // NOTE: @[conflict, rev, other]
            if(counter == 0){
                REQUIRE(doc->docID == docID);
                REQUIRE(doc->selectedRev.revID == history[0]);
                REQUIRE(c4SliceEqual(doc->selectedRev.body, body));
            }else if(counter == 1){
                REQUIRE(doc->docID == docID);
                REQUIRE(doc->selectedRev.revID == conflictHistory[0]);
                REQUIRE(c4SliceEqual(doc->selectedRev.body, conflictBody));
            }else if(counter == 2){
                REQUIRE(doc->docID == otherDocID);
                REQUIRE(doc->selectedRev.revID == otherHistory[0]);
                REQUIRE(c4SliceEqual(doc->selectedRev.body, otherBody));
            }
            counter++;
        }while(c4doc_selectNextLeafRevision(doc, true, true, &error));
        c4doc_free(doc);
    }
    c4enum_free(e);
    REQUIRE(counter == 3);
    

    // Verify that compaction leaves the document history:
    // TODO: compact() is not fully implemented
//    error = {};
//    REQUIRE(c4db_compact(db, &error));

    // Delete the current winning rev, leaving the other one:
    doc = putDoc(docID, conflictHistory[0], kC4SliceNull, kRevDeleted);
    c4doc_free(doc);
    doc = getDoc(docID);
    //TODO: Uncomment once https://github.com/couchbase/couchbase-lite-core/issues/57 is fixed
    //REQUIRE(doc->revID == history[0]); // 4-4444 should be current??
    //REQUIRE(doc->selectedRev.revID == history[0]);
    //verifyRev(doc, history, historyCount, body);
    c4doc_free(doc);
    
    // Delete the remaining rev:
    doc = putDoc(docID, history[0], kC4SliceNull, kRevDeleted);
    c4doc_free(doc);
    // TODO: Need to implement following tests
}

// test07_RevTreeConflict
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "RevTreeConflict", "[Database][C]") {
    if(!isRevTrees()) return;
    
    // Track the latest database-change notification that's posted:
    
    // TODO: Observer
    
    C4String docID = C4STR("MyDocID");
    C4String body = C4STR("{\"message\":\"hi\"}");
    const size_t historyCount = 1;
    const C4String history[historyCount] = {C4STR("1-1111")};
    C4Document* doc = forceInsert(db, docID, history, historyCount, body, 0);
    REQUIRE(c4db_getDocumentCount(db) == 1);
    verifyRev(doc, history, historyCount, body);
    c4doc_free(doc);
    
    const size_t newHistoryCount = 3;
    const C4String newHistory[newHistoryCount] = {C4STR("3-3333"), C4STR("2-2222"), C4STR("1-1111")};
    doc = forceInsert(db, docID, newHistory, newHistoryCount, body, 0);
    REQUIRE(c4db_getDocumentCount(db) == 1);
    verifyRev(doc, newHistory, newHistoryCount, body);
    c4doc_free(doc);
}

// test08_DeterministicRevIDs
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "DeterministicRevIDs", "[Database][C]") {
    if(!isRevTrees()) return;
    
    C4String docID = C4STR("mydoc");
    C4String body = C4STR("{\"key\":\"value\"}");
    C4Document* doc = putDoc(docID, kC4SliceNull, body);
    C4String revID = copy(doc->revID);
    c4doc_free(doc);
    
    deleteAndRecreateDB();
    
    doc = putDoc(docID, kC4SliceNull, body);
    REQUIRE(doc->revID == revID);
    REQUIRE(doc->selectedRev.revID == revID);
    c4doc_free(doc);
    
    free(revID);
}

// test09_DuplicateRev
N_WAY_TEST_CASE_METHOD(C4DatabaseInternalTest, "DuplicateRev", "[Database][C]") {
    if(!isRevTrees()) return;
    
    // rev1
    C4String docID = C4STR("mydoc");
    C4String body = C4STR("{\"key\":\"value\"}");
    C4Document* doc = putDoc(docID, kC4SliceNull, body);
    C4String revID = copy(doc->revID);
    c4doc_free(doc);
    
    // rev2a
    body = C4STR("{\"key\":\"new-value\"}");
    doc = putDoc(docID, revID, body);
    C4String revID2a = copy(doc->revID);
    c4doc_free(doc);
    
    // rev2b
    {
        TransactionHelper t(db);
        C4Slice history[1] = {revID};
        C4DocPutRequest rq = {};
        rq.allowConflict = true;
        rq.docID = docID;
        rq.history = history;
        rq.historyCount = 1;
        rq.body = body;
        rq.revFlags = 0;
        rq.save = true;
        C4Error error = {};
        doc = c4doc_put(db, &rq, nullptr, &error);
        REQUIRE(doc->docID == docID);
        REQUIRE(error.domain == 0);
        REQUIRE(error.code == 0);
    }
    C4String revID2b = copy(doc->revID);
    c4doc_free(doc);

    REQUIRE(revID2a == revID2b);
    
    free(revID);
    free(revID2a);
    free(revID2b);
}

#pragma mark - MISC.:

// test18_FindMissingRevisions
// test23_MakeRevisionHistoryDict
// test25_FileProtection
// test27_ChangesSinceSequence
// test29_autoPruneOnPut
// test29_autoPruneOnForceInsert
// test30_conflictAfterPrune
