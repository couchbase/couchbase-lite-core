//
//  c4DatabaseTest.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/14/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

#include "c4Test.hh"
#include "c4Private.h"
#include "c4DocEnumerator.h"
#include "c4ExpiryEnumerator.h"
#include <cmath>

#include "forestdb.h"   // needed for error codes
#include "sqlite3.h"

#ifdef _MSC_VER
#include <ctime>
#include "Windows.h"
#define sleep(sec) Sleep((sec)*1000)
#else
#include "unistd.h"
#endif


class C4DatabaseTest : public C4Test {
    public:

    C4DatabaseTest(int testOption) :C4Test(testOption) { }

    void assertMessage(C4ErrorDomain domain, int code, const char *expectedMsg) {
        C4SliceResult msg = c4error_getMessage({domain, code});
        REQUIRE(std::string((char*)msg.buf, msg.size) == std::string(expectedMsg));
        c4slice_free(msg);

        char buf[256];
        char *cmsg = c4error_getMessageC({domain, code}, buf, sizeof(buf));
        REQUIRE(std::string(cmsg) == std::string(expectedMsg));
        REQUIRE(cmsg == &buf[0]);
    }

    void setupAllDocs() {
        char docID[20];
        for (int i = 1; i < 100; i++) {
            sprintf(docID, "doc-%03d", i);
            createRev(c4str(docID), kRevID, kBody);
        }
        // Add a deleted doc to make sure it's skipped by default:
        createRev(c4str("doc-005DEL"), kRevID, kC4SliceNull);
    }
};


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database ErrorMessages", "[Database][C]") {
    C4SliceResult msg = c4error_getMessage({ForestDBDomain, 0});
    REQUIRE(msg.buf == (const void*)nullptr);
    REQUIRE((unsigned long)msg.size == 0ul);

    char buf[256];
    char *cmsg = c4error_getMessageC({ForestDBDomain, 0}, buf, sizeof(buf));
    REQUIRE(cmsg == &buf[0]);
    REQUIRE(buf[0] == '\0');

    assertMessage(ForestDBDomain, FDB_RESULT_KEY_NOT_FOUND, "key not found");
    assertMessage(SQLiteDomain, SQLITE_CORRUPT, "database disk image is malformed");
    assertMessage(LiteCoreDomain, 15, "invalid parameter");
    assertMessage(POSIXDomain, ENOENT, "No such file or directory");
    assertMessage(LiteCoreDomain, kC4ErrorIndexBusy, "index busy; can't close view");
    assertMessage(ForestDBDomain, -1234, "unknown error");
    assertMessage((C4ErrorDomain)666, -1234, "unknown error domain");
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database OpenBundle", "[Database][C][!throws]") {
    auto config = *c4db_getConfig(db);
    config.flags |= kC4DB_Bundled;

    C4Slice bundlePath = c4str(kTestDir "cbl_core_test_bundle");
    c4db_deleteAtPath(bundlePath, &config, nullptr);
    C4Error error;
    auto bundle = c4db_open(bundlePath, &config, &error);
    REQUIRE(bundle);
    C4SliceResult path = c4db_getPath(bundle);
    REQUIRE(path == c4str(kTestDir "cbl_core_test_bundle/")); // note trailing '/'
    c4slice_free(path);
    REQUIRE(c4db_close(bundle, &error));
    c4db_free(bundle);

    // Reopen without 'create' flag:
    config.flags &= ~kC4DB_Create;
    bundle = c4db_open(bundlePath, &config, &error);
    REQUIRE(bundle);
    REQUIRE(c4db_close(bundle, &error));
    c4db_free(bundle);

    // Reopen with wrong storage type:
    c4log_warnOnErrors(false);
    if (config.storageEngine == kC4SQLiteStorageEngine)
        config.storageEngine = kC4ForestDBStorageEngine;
    else
        config.storageEngine = kC4SQLiteStorageEngine;
    REQUIRE(!c4db_open(bundlePath, &config, &error));

    // Open nonexistent bundle:
    REQUIRE(!c4db_open(c4str(kTestDir "no_such_bundle"), &config, &error));
    c4log_warnOnErrors(true);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Transaction", "[Database][C]") {
    REQUIRE(c4db_getDocumentCount(db) == (C4SequenceNumber)0);
    REQUIRE(!c4db_isInTransaction(db));
    C4Error(error);
    REQUIRE(c4db_beginTransaction(db, &error));
    REQUIRE(c4db_isInTransaction(db));
    REQUIRE(c4db_beginTransaction(db, &error));
    REQUIRE(c4db_isInTransaction(db));
    REQUIRE(c4db_endTransaction(db, true, &error));
    REQUIRE(c4db_isInTransaction(db));
    REQUIRE(c4db_endTransaction(db, true, &error));
    REQUIRE(!c4db_isInTransaction(db));
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database CreateRawDoc", "[Database][C]") {
    const C4Slice key = c4str("key");
    const C4Slice meta = c4str("meta");
    C4Error error;
    REQUIRE(c4db_beginTransaction(db, &error));
    c4raw_put(db, c4str("test"), key, meta, kBody, &error);
    REQUIRE(c4db_endTransaction(db, true, &error));

    C4RawDocument *doc = c4raw_get(db, c4str("test"), key, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->key == key);
    REQUIRE(doc->meta == meta);
    REQUIRE(doc->body == kBody);
    c4raw_free(doc);

    // Nonexistent:
    REQUIRE(c4raw_get(db, c4str("test"), c4str("bogus"), &error) == (C4RawDocument*)nullptr);
    REQUIRE(error.domain == LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database CreateVersionedDoc", "[Database][C]") {
    // Try reading doc with mustExist=true, which should fail:
    C4Error error;
    C4Document* doc;
    doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(!doc);
    REQUIRE((uint32_t)error.domain == (uint32_t)LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
    c4doc_free(doc);

    // Now get the doc with mustExist=false, which returns an empty doc:
    doc = c4doc_get(db, kDocID, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == (C4DocumentFlags)0);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID.buf == 0);
    REQUIRE(doc->selectedRev.revID.buf == 0);
    c4doc_free(doc);

    {
        TransactionHelper t(db);
        C4DocPutRequest rq = {};
        rq.existingRevision = true;
        rq.docID = kDocID;
        rq.history = &kRevID;
        rq.historyCount = 1;
        rq.body = kBody;
        rq.save = true;
        doc = c4doc_put(db, &rq, nullptr, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(doc->revID == kRevID);
        REQUIRE(doc->selectedRev.revID == kRevID);
        REQUIRE(doc->selectedRev.flags == (C4RevisionFlags)kRevLeaf);
        REQUIRE(doc->selectedRev.body == kBody);
        c4doc_free(doc);
    }

    // Reload the doc:
    doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)1);
    REQUIRE(doc->selectedRev.body == kBody);
    c4doc_free(doc);

    // Get the doc by its sequence:
    doc = c4doc_getBySequence(db, 1, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->selectedRev.revID == kRevID);
    REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)1);
    REQUIRE(doc->selectedRev.body == kBody);
    c4doc_free(doc);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database CreateMultipleRevisions", "[Database][C]") {
    const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
    createRev(kDocID, kRevID, kBody);
    createRev(kDocID, kRev2ID, kBody2);
    createRev(kDocID, kRev2ID, kBody2, false); // test redundant insert

    // Reload the doc:
    C4Error error;
    C4Document *doc = c4doc_get(db, kDocID, true, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRev2ID);
    REQUIRE(doc->selectedRev.revID == kRev2ID);
    REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)2);
    REQUIRE(doc->selectedRev.body == kBody2);

    if (versioning() == kC4RevisionTrees) {
        // Select 1st revision:
        REQUIRE(c4doc_selectParentRevision(doc));
        REQUIRE(doc->selectedRev.revID == kRevID);
        REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)1);
        REQUIRE(doc->selectedRev.body == kC4SliceNull);
        REQUIRE(c4doc_hasRevisionBody(doc));
        REQUIRE(c4doc_loadRevisionBody(doc, &error)); // have to explicitly load the body
        REQUIRE(doc->selectedRev.body == kBody);
        REQUIRE(!c4doc_selectParentRevision(doc));
        c4doc_free(doc);

        // Compact database:
        REQUIRE(c4db_compact(db, &error));

        doc = c4doc_get(db, kDocID, true, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(c4doc_selectParentRevision(doc));
        REQUIRE(doc->selectedRev.revID == kRevID);
        REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)1);
        if (!isSQLite()) {
            REQUIRE(doc->selectedRev.body == kC4SliceNull);
            REQUIRE(!c4doc_hasRevisionBody(doc));
            REQUIRE(!c4doc_loadRevisionBody(doc, &error));
        }

        // Purge doc
        {
            TransactionHelper t(db);
            int nPurged = c4doc_purgeRevision(doc, kRev2ID, &error);
            REQUIRE(nPurged == 2);
            REQUIRE(c4doc_save(doc, 20, &error));
        }
    }
    c4doc_free(doc);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database GetForPut", "[Database][C]") {
    C4Error error;
    TransactionHelper t(db);

    // Creating doc given ID:
    auto doc = c4doc_getForPut(db, kDocID, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kC4SliceNull);
    REQUIRE(doc->flags == (C4DocumentFlags)0);
    REQUIRE(doc->selectedRev.revID == kC4SliceNull);
    c4doc_free(doc);

    // Creating doc, no ID:
    doc = c4doc_getForPut(db, kC4SliceNull, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID.size >= 20);  // Verify it got a random doc ID
    REQUIRE(doc->revID == kC4SliceNull);
    REQUIRE(doc->flags == (C4DocumentFlags)0);
    REQUIRE(doc->selectedRev.revID == kC4SliceNull);
    c4doc_free(doc);

    // Delete with no revID given
    doc = c4doc_getForPut(db, kDocID, kC4SliceNull, true/*deleting*/, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorNotFound);

    // Adding new rev of nonexistent doc:
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorNotFound);

    // Adding new rev of existing doc:
    createRev(kDocID, kRevID, kBody);
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRevID);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->selectedRev.revID == kRevID);
    c4doc_free(doc);

    // Adding new rev, with nonexistent parent:
    doc = c4doc_getForPut(db, kDocID, kRev2ID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorConflict);

    // Conflict -- try & fail to update non-current rev:
    const C4Slice kBody2 = C4STR("{\"ok\":\"go\"}");
    createRev(kDocID, kRev2ID, kBody2);
    doc = c4doc_getForPut(db, kDocID, kRevID, false, false, &error);
    REQUIRE(doc == nullptr);
    REQUIRE(error.code == kC4ErrorConflict);

    if (isRevTrees()) {
        // Conflict -- force an update of non-current rev:
        doc = c4doc_getForPut(db, kDocID, kRevID, false, true/*allowConflicts*/, &error);
        REQUIRE(doc != nullptr);
        REQUIRE(doc->docID == kDocID);
        REQUIRE(doc->selectedRev.revID == kRevID);
        c4doc_free(doc);
    }

    // Deleting the doc:
    doc = c4doc_getForPut(db, kDocID, kRev2ID, true/*deleted*/, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->selectedRev.revID == kRev2ID);
    c4doc_free(doc);
    
    // Actually delete it:
    createRev(kDocID, kRev3ID, kC4SliceNull);

    // Re-creating the doc (no revID given):
    doc = c4doc_getForPut(db, kDocID, kC4SliceNull, false, false, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    REQUIRE(doc->revID == kRev3ID);
    REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kDeleted));
    REQUIRE(doc->selectedRev.revID == kRev3ID);
    c4doc_free(doc);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Put", "[Database][C]") {
    C4Error error;
    TransactionHelper t(db);

    // Creating doc given ID:
    C4DocPutRequest rq = {};
    rq.docID = kDocID;
    rq.body = kBody;
    rq.save = true;
    auto doc = c4doc_put(db, &rq, nullptr, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->docID == kDocID);
    C4Slice kExpectedRevID = isRevTrees() ? C4STR("1-c10c25442d9fe14fa3ca0db4322d7f1e43140fab")
                                          : C4STR("1@*");
    REQUIRE(doc->revID == kExpectedRevID);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->selectedRev.revID == kExpectedRevID);
    c4doc_free(doc);

    // Update doc:
    rq.body = C4STR("{\"ok\":\"go\"}");
    rq.history = &kExpectedRevID;
    rq.historyCount = 1;
    size_t commonAncestorIndex;
    doc = c4doc_put(db, &rq, &commonAncestorIndex, &error);
    REQUIRE(doc != nullptr);
    REQUIRE((unsigned long)commonAncestorIndex == 1ul);
    C4Slice kExpectedRev2ID = isRevTrees() ? C4STR("2-32c711b29ea3297e27f3c28c8b066a68e1bb3f7b")
                                           : C4STR("2@*");
    REQUIRE(doc->revID == kExpectedRev2ID);
    REQUIRE(doc->flags == (C4DocumentFlags)kExists);
    REQUIRE(doc->selectedRev.revID == kExpectedRev2ID);
    c4doc_free(doc);

    // Insert existing rev that conflicts:
    rq.body = C4STR("{\"from\":\"elsewhere\"}");
    rq.existingRevision = true;
    C4Slice kConflictRevID = isRevTrees() ? C4STR("2-deadbeef")
                                          : C4STR("1@binky");
    C4Slice history[2] = {kConflictRevID, kExpectedRevID};
    rq.history = history;
    rq.historyCount = 2;
    doc = c4doc_put(db, &rq, &commonAncestorIndex, &error);
    REQUIRE(doc != nullptr);
    REQUIRE((unsigned long)commonAncestorIndex == 1ul);
    REQUIRE(doc->selectedRev.revID == kConflictRevID);
    REQUIRE(doc->flags == (C4DocumentFlags)(kExists | kConflicted));
    if (isRevTrees())
        REQUIRE(doc->revID == kConflictRevID);
    else
        REQUIRE(doc->revID == kExpectedRev2ID);
    c4doc_free(doc);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database AllDocs", "[Database][C]") {
    setupAllDocs();
    C4Error error;
    C4DocEnumerator* e;

    REQUIRE(c4db_getDocumentCount(db) == 99ull);

    // No start or end ID:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    options.flags &= ~kC4IncludeBodies;
    e = c4db_enumerateAllDocs(db, kC4SliceNull, kC4SliceNull, &options, &error);
    REQUIRE(e);
    char docID[20];
    int i = 1;
    while (c4enum_next(e, &error)) {
        auto doc = c4enum_getDocument(e, &error);
        REQUIRE(doc);
        sprintf(docID, "doc-%03d", i);
        REQUIRE(doc->docID == c4str(docID));
        REQUIRE(doc->revID == kRevID);
        REQUIRE(doc->selectedRev.revID == kRevID);
        REQUIRE(doc->selectedRev.sequence == (C4SequenceNumber)i);
        REQUIRE(doc->selectedRev.body == kC4SliceNull);
        // Doc was loaded without its body, but it should load on demand:
        REQUIRE(c4doc_loadRevisionBody(doc, &error)); // have to explicitly load the body
        REQUIRE(doc->selectedRev.body == kBody);

        C4DocumentInfo info;
        REQUIRE(c4enum_getDocumentInfo(e, &info));
        REQUIRE(info.docID == c4str(docID));
        REQUIRE(info.revID == kRevID);

        c4doc_free(doc);
        i++;
    }
    c4enum_free(e);
    REQUIRE(i == 100);

    // Start and end ID:
    e = c4db_enumerateAllDocs(db, c4str("doc-007"), c4str("doc-090"), nullptr, &error);
    REQUIRE(e);
    i = 7;
    while (c4enum_next(e, &error)) {
        auto doc = c4enum_getDocument(e, &error);
        REQUIRE(error.code == 0);
        REQUIRE(doc);
        sprintf(docID, "doc-%03d", i);
        REQUIRE(doc->docID == c4str(docID));
        c4doc_free(doc);
        i++;
    }
    c4enum_free(e);
    REQUIRE(i == 91);

    // Some docs, by ID:
    options = kC4DefaultEnumeratorOptions;
    options.flags |= kC4IncludeDeleted;
    C4Slice docIDs[4] = {C4STR("doc-042"), C4STR("doc-007"), C4STR("bogus"), C4STR("doc-001")};
    e = c4db_enumerateSomeDocs(db, docIDs, 4, &options, &error);
    REQUIRE(e);
    i = 0;
    while (c4enum_next(e, &error)) {
        auto doc = c4enum_getDocument(e, &error);
        REQUIRE(error.code == 0);
        REQUIRE(doc);
        REQUIRE(doc->docID == docIDs[i]);
        REQUIRE((doc->sequence != 0) == (i != 2));
        c4doc_free(doc);
        i++;
    }
    c4enum_free(e);
    REQUIRE(i == 4);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database AllDocsIncludeDeleted", "[Database][C]") {
    char docID[20];
    setupAllDocs();

    C4Error error;
    C4DocEnumerator* e;

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    options.flags |= kC4IncludeDeleted;
    e = c4db_enumerateAllDocs(db, c4str("doc-004"), c4str("doc-007"), &options, &error);
    REQUIRE(e);
    int i = 4;
    while (c4enum_next(e, &error)) {
        auto doc = c4enum_getDocument(e, &error);
        REQUIRE(doc);
        if (i == 6)
            strncpy(docID, "doc-005DEL", sizeof(docID));
        else
            sprintf(docID, "doc-%03d", i - (i>=6));
        REQUIRE(doc->docID == c4str(docID));
        c4doc_free(doc);
        i++;
    }
    c4enum_free(e);
    REQUIRE(i == 9);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database AllDocsInfo", "[Database][C]") {
    setupAllDocs();
    C4Error error;
    C4DocEnumerator* e;

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    e = c4db_enumerateAllDocs(db, kC4SliceNull, kC4SliceNull, &options, &error);
    REQUIRE(e);
    int i = 1;
    while(c4enum_next(e, &error)) {
        C4DocumentInfo doc;
        REQUIRE(c4enum_getDocumentInfo(e, &doc));
        char docID[20];
        sprintf(docID, "doc-%03d", i);
        REQUIRE(doc.docID == c4str(docID));
        REQUIRE(doc.revID == kRevID);
        REQUIRE(doc.sequence == (uint64_t)i);
        REQUIRE(doc.flags == (C4DocumentFlags)kExists);
        i++;
    }
    c4enum_free(e);
    REQUIRE(error.code == 0);
    REQUIRE(i == 100);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Changes", "[Database][C]") {
    char docID[20];
    for (int i = 1; i < 100; i++) {
        sprintf(docID, "doc-%03d", i);
        createRev(c4str(docID), kRevID, kBody);
    }

    C4Error error;
    C4DocEnumerator* e;
    C4Document* doc;

    // Since start:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    options.flags &= ~kC4IncludeBodies;
    e = c4db_enumerateChanges(db, 0, &options, &error);
    REQUIRE(e);
    C4SequenceNumber seq = 1;
    while (nullptr != (doc = c4enum_nextDocument(e, &error))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        sprintf(docID, "doc-%03llu", seq);
        REQUIRE(doc->docID == c4str(docID));
        c4doc_free(doc);
        seq++;
    }
    c4enum_free(e);

    // Since 6:
    e = c4db_enumerateChanges(db, 6, &options, &error);
    REQUIRE(e);
    seq = 7;
    while (nullptr != (doc = c4enum_nextDocument(e, &error))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        sprintf(docID, "doc-%03llu", seq);
        REQUIRE(doc->docID == c4str(docID));
        c4doc_free(doc);
        seq++;
    }
    c4enum_free(e);
    REQUIRE(seq == (C4SequenceNumber)100);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Expired", "[Database][C]") {
    C4Slice docID = C4STR("expire_me");
    createRev(docID, kRevID, kBody);
    time_t expire = time(nullptr) + 1;
    C4Error err;
    REQUIRE(c4doc_setExpiration(db, docID, expire, &err));
    
    expire = time(nullptr) + 2;
    // Make sure setting it to the same is also true
    REQUIRE(c4doc_setExpiration(db, docID, expire, &err));
    REQUIRE(c4doc_setExpiration(db, docID, expire, &err));
    
    C4Slice docID2 = C4STR("expire_me_too");
    createRev(docID2, kRevID, kBody);
    REQUIRE(c4doc_setExpiration(db, docID2, expire, &err));

    C4Slice docID3 = C4STR("dont_expire_me");
    createRev(docID3, kRevID, kBody);
    sleep(2u);
    
    C4ExpiryEnumerator *e = c4db_enumerateExpired(db, &err);
    REQUIRE(e != nullptr);
    
    int expiredCount = 0;
    while(c4exp_next(e, nullptr)) {
        C4SliceResult existingDocID = c4exp_getDocID(e);
        REQUIRE(existingDocID != docID3);
        c4slice_free(existingDocID);
        expiredCount++;
    }
    
    c4exp_free(e);
    REQUIRE(expiredCount == 2);
    REQUIRE(c4doc_getExpiration(db, docID) == (uint64_t)expire);
    REQUIRE(c4doc_getExpiration(db, docID2) == (uint64_t)expire);
    REQUIRE(c4db_nextDocExpiration(db) == (uint64_t)expire);
    
    e = c4db_enumerateExpired(db, &err);
    REQUIRE(e != nullptr);
    
    expiredCount = 0;
    while(c4exp_next(e, nullptr)) {
        C4SliceResult existingDocID = c4exp_getDocID(e);
        REQUIRE(existingDocID != docID3);
        c4slice_free(existingDocID);
        expiredCount++;
    }
    
    REQUIRE(c4exp_purgeExpired(e, &err));
    c4exp_free(e);
    REQUIRE(expiredCount == 2);
    
    e = c4db_enumerateExpired(db, &err);
    REQUIRE(e != nullptr);
    
    expiredCount = 0;
    while(c4exp_next(e, nullptr)) {
        expiredCount++;
    }
    
    REQUIRE(c4exp_purgeExpired(e, &err));
    c4exp_free(e);
    REQUIRE(expiredCount == 0);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database CancelExpire", "[Database][C]")
{
    C4Slice docID = C4STR("expire_me");
    createRev(docID, kRevID, kBody);
    time_t expire = time(nullptr) + 2;
    C4Error err;
    REQUIRE(c4doc_setExpiration(db, docID, expire, &err));
    REQUIRE(c4doc_setExpiration(db, docID, UINT64_MAX, &err));
    
    sleep(2u);
    auto e = c4db_enumerateExpired(db, &err);
    REQUIRE(e != nullptr);
    
    int expiredCount = 0;
    while(c4exp_next(e, nullptr)) {
        expiredCount++;
    }
    
    REQUIRE(c4exp_purgeExpired(e, &err));
    c4exp_free(e);
    REQUIRE(expiredCount == 0);
}
