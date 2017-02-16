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
    C4Document* doc;
    C4String docID;
    C4String revID1;
    C4String revID2;
    C4String revID3;
    C4String body = C4STR("{\"foo\":1, \"bar\":false}");
    C4String updatedBody = C4STR("{\"foo\":1, \"bar\":false, \"status\":\"updated!\"}");
    
    // Make sure the database-changed notifications have the right data in them (see issue #93)
    
    // Get a nonexistent document:
    REQUIRE(c4doc_get(db, C4STR("nonexistent"), true, &c4err) == NULL);
    REQUIRE(c4err.domain == LiteCoreDomain);
    REQUIRE(c4err.code == kC4ErrorNotFound);
    
    // Create a document:
    {
        TransactionHelper t(db);
        C4DocPutRequest rq = {
            .docID = kC4SliceNull,
            .body = body,
            .revFlags = kRevKeepBody,// Revision's body should not be discarded when non-leaf
            .save = true };
        doc = c4doc_put(db, &rq, nullptr, &c4err);
        REQUIRE(doc);
        printf("docID: %s\n", C4STR_TO_CSTR(doc->docID));
        printf("revID1: %s\n", C4STR_TO_CSTR(doc->revID));
        REQUIRE(doc->docID.size >= 10);
        REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "1-") == 0);
        docID = copy(doc->docID);
        revID1 = copy(doc->revID);
        c4doc_free(doc);
        printf("docID: %s\n", C4STR_TO_CSTR(docID));
    }
    
    // Read it back:
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->selectedRev.revID == revID1);
    REQUIRE(doc->selectedRev.body == body);
    c4doc_free(doc);
    
    // Now update it:
    {
        TransactionHelper t(db);
        C4Slice history[1] = {revID1};
        C4DocPutRequest rq = {
            .docID = docID,
            .history = history,
            .historyCount = 1,
            .body = updatedBody,
            .revFlags = kRevKeepBody,// Revision's body should not be discarded when non-leaf
            .save = true };
        size_t commonAncestorIndex;
        doc = c4doc_put(db, &rq, &commonAncestorIndex, &c4err);
        REQUIRE(doc);
        printf("revID2: %s\n", C4STR_TO_CSTR(doc->revID));
        REQUIRE(doc->docID == docID);
        REQUIRE(doc->selectedRev.body == updatedBody);
        REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "2-") == 0);
        revID2 = copy(doc->revID);
        c4doc_free(doc);
    }
    
    // Read it back:
    doc = c4doc_get(db, docID, true, &c4err);
    REQUIRE(doc);
    REQUIRE(doc->docID == docID);
    REQUIRE(doc->selectedRev.revID == revID2);
    REQUIRE(doc->selectedRev.body == updatedBody);
    c4doc_free(doc);
    
    // Try to update the first rev, which should fail:
    {
        TransactionHelper t(db);
        C4Slice history[1] = {revID1};
        C4DocPutRequest rq = {
            .docID = docID,
            .allowConflict = false,
            .history = history,
            .historyCount = 1,
            .body = updatedBody,
            .revFlags = kRevKeepBody,// Revision's body should not be discarded when non-leaf
            .save = true };
        size_t commonAncestorIndex;
        doc = c4doc_put(db, &rq, &commonAncestorIndex, &c4err);
        REQUIRE(doc == nullptr);
        REQUIRE(c4err.domain == LiteCoreDomain);
        REQUIRE(c4err.code == kC4ErrorConflict);
    }
    
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
    
    // NOTE: Filter is out of LiteCore
    
    // Delete it:
    
    // without previous revision ID -> error
    {
        TransactionHelper t(db);
        c4err = {};
        C4DocPutRequest rq = {
            .docID = docID,
            .body = kC4SliceNull,
            .revFlags = kRevDeleted,
            .save = true };
        doc = c4doc_put(db, &rq, nullptr, &c4err);
        REQUIRE(!doc);
        REQUIRE(c4err.domain == LiteCoreDomain);
        REQUIRE(c4err.code == kC4ErrorConflict);
    }
    
    // with previous revision ID -> success
    {
        TransactionHelper t(db);
        c4err = {};
        C4Slice history[1] = {revID2};
        C4DocPutRequest rq = {
            .docID = docID,
            .history = history,
            .historyCount = 1,
            .body = kC4SliceNull,
            .revFlags = kRevDeleted,
            .save = true };
        doc = c4doc_put(db, &rq, nullptr, &c4err);
        REQUIRE(doc);
        REQUIRE(c4err.domain == 0);
        REQUIRE(c4err.code == 0);
        REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kDeleted));
        REQUIRE(doc->docID == docID);
        REQUIRE(C4STR_TO_STDSTR(doc->revID).compare(0, 2, "3-") == 0);
        printf("revID3: %s\n", C4STR_TO_CSTR(doc->revID));
        printf("flags: 0x%x\n", doc->flags);
        revID3 = copy(doc->revID);
        c4doc_free(doc);
    }
    
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
    {
        TransactionHelper t(db);
        c4err = {};
        C4DocPutRequest rq = {
            .docID = C4STR("fake"),
            .revFlags = kRevDeleted,
            .save = true };
        doc = c4doc_put(db, &rq, nullptr, &c4err);
        REQUIRE(!doc);
        REQUIRE(c4err.domain == LiteCoreDomain);
        REQUIRE(c4err.code == kC4ErrorNotFound);
    }
    
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
    // NOTE: current implementation: c4db_compact() deletes only deleted revisions
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
// test02_ExpectedRevIDs
// test03_DeleteWithProperties
// test04_DeleteAndRecreate
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
