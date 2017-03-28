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
        createNumberedDocs(99);
        // Add a deleted doc to make sure it's skipped by default:
        createRev(c4str("doc-005DEL"), kRevID, kC4SliceNull, kRevDeleted);
    }
};


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database ErrorMessages", "[Database][C]") {
    C4SliceResult msg = c4error_getMessage({LiteCoreDomain, 0});
    REQUIRE(msg.buf == (const void*)nullptr);
    REQUIRE((unsigned long)msg.size == 0ul);

    char buf[256];
    char *cmsg = c4error_getMessageC({LiteCoreDomain, 0}, buf, sizeof(buf));
    REQUIRE(cmsg == &buf[0]);
    REQUIRE(buf[0] == '\0');

    assertMessage(SQLiteDomain, SQLITE_CORRUPT, "database disk image is malformed");
    assertMessage(LiteCoreDomain, 15, "invalid parameter");
    assertMessage(POSIXDomain, ENOENT, "No such file or directory");
    assertMessage(LiteCoreDomain, kC4ErrorIndexBusy, "index busy; can't close view");
    assertMessage(SQLiteDomain, -1234, "unknown error");
    assertMessage((C4ErrorDomain)666, -1234, "unknown error domain");
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Info", "[Database][C]") {
    REQUIRE(c4db_getDocumentCount(db) == 0);
    REQUIRE(c4db_getLastSequence(db) == 0);
    C4Error err;
    C4UUID publicUUID, privateUUID;
    REQUIRE(c4db_getUUIDs(db, &publicUUID, &privateUUID, &err));
    REQUIRE(memcmp(&publicUUID, &privateUUID, sizeof(C4UUID)) != 0);
    // Weird requirements of UUIDs according to the spec:
    REQUIRE((publicUUID.bytes[6] & 0xF0) == 0x40);
    REQUIRE((publicUUID.bytes[8] & 0xC0) == 0x80);
    REQUIRE((privateUUID.bytes[6] & 0xF0) == 0x40);
    REQUIRE((privateUUID.bytes[8] & 0xC0) == 0x80);

    // Make sure UUIDs are persistent:
    reopenDB();
    C4UUID publicUUID2, privateUUID2;
    REQUIRE(c4db_getUUIDs(db, &publicUUID2, &privateUUID2, &err));
    REQUIRE(memcmp(&publicUUID, &publicUUID2, sizeof(C4UUID)) == 0);
    REQUIRE(memcmp(&privateUUID, &privateUUID2, sizeof(C4UUID)) == 0);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database OpenBundle", "[Database][C][!throws]") {
    auto config = *c4db_getConfig(db);
    config.flags |= kC4DB_Bundled;

    std::string bundlePathStr = TempDir() + "cbl_core_test_bundle";
    C4Slice bundlePath = c4str(bundlePathStr.c_str());
    C4Error error;
    if (!c4db_deleteAtPath(bundlePath, &config, &error))
        REQUIRE(error.code == 0);
    auto bundle = c4db_open(bundlePath, &config, &error);
    REQUIRE(bundle);
    C4SliceResult path = c4db_getPath(bundle);
    REQUIRE(path == TEMPDIR("cbl_core_test_bundle" kPathSeparator)); // note trailing '/'
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
    auto engine = config.storageEngine;
    config.storageEngine = "b0gus";
    REQUIRE(!c4db_open(bundlePath, &config, &error));
    config.storageEngine = engine;

    // Open nonexistent bundle:
    REQUIRE(!c4db_open(TEMPDIR("no_such_bundle"), &config, &error));
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


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database AllDocs", "[Database][C]") {
    setupAllDocs();
    C4Error error;
    C4DocEnumerator* e;

    REQUIRE(c4db_getDocumentCount(db) == 99);

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
        REQUIRE(info.flags == kExists);
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
    createNumberedDocs(99);

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
        char docID[30];
        sprintf(docID, "doc-%03llu", (unsigned long long)seq);
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
        char docID[30];
        sprintf(docID, "doc-%03llu", (unsigned long long)seq);
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

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database BlobStore", "[Database][C]")
{
    C4Error err;
    C4BlobStore *blobs = c4db_getBlobStore(db, &err);
    REQUIRE(blobs != nullptr);
}
