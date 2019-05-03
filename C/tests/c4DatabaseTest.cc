//
// c4DatabaseTest.cc
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

using namespace std;

static C4Document* c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError) noexcept {
    return c4enum_next(e, outError) ? c4enum_getDocument(e, outError) : nullptr;
}

class C4DatabaseTest : public C4Test {
    public:

    C4DatabaseTest(int testOption) :C4Test(testOption) { }

    void assertMessage(C4ErrorDomain domain, int code,
                       const char *expectedDomainAndType, const char *expectedMsg) {
        C4SliceResult msg = c4error_getMessage({domain, code});
        CHECK(std::string((char*)msg.buf, msg.size) == std::string(expectedMsg));
        c4slice_free(msg);

        string expectedDesc = string(expectedDomainAndType) + " \"" + expectedMsg + "\"";
        char buf[256];
        char *cmsg = c4error_getDescriptionC({domain, code}, buf, sizeof(buf));
        CHECK(std::string(cmsg) == expectedDesc);
        CHECK(cmsg == &buf[0]);
    }

    void setupAllDocs() {
        createNumberedDocs(99);
        // Add a deleted doc to make sure it's skipped by default:
        createRev(c4str("doc-005DEL"), kRevID, kC4SliceNull, kRevDeleted);
    }
};


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database ErrorMessages", "[Database][C]") {
    alloc_slice msg = c4error_getMessage({LiteCoreDomain, 0});
    REQUIRE(msg.buf == (const void*)nullptr);
    REQUIRE((unsigned long)msg.size == 0ul);

    msg = c4error_getDescription({LiteCoreDomain, 0});
    REQUIRE(msg == "No error"_sl);

    char buf[256];
    char *cmsg = c4error_getDescriptionC({LiteCoreDomain, 0}, buf, sizeof(buf));
    REQUIRE(cmsg == &buf[0]);
    REQUIRE(strcmp(cmsg, "No error") == 0);

    assertMessage(SQLiteDomain, SQLITE_CORRUPT, "SQLite error 11", "database disk image is malformed");
    assertMessage(SQLiteDomain, SQLITE_IOERR_ACCESS, "SQLite error 3338", "disk I/O error (3338)");
    assertMessage(SQLiteDomain, SQLITE_IOERR, "SQLite error 10", "disk I/O error");
    assertMessage(LiteCoreDomain, 15, "LiteCore error 15", "data is corrupted");
    assertMessage(POSIXDomain, ENOENT, "POSIX error 2", "No such file or directory");
    assertMessage(LiteCoreDomain, kC4ErrorTransactionNotClosed, "LiteCore error 18", "transaction not closed");
    assertMessage(SQLiteDomain, -1234, "SQLite error -1234", "unknown error (-1234)");
    assertMessage((C4ErrorDomain)666, -1234, "INVALID_DOMAIN error -1234", "unknown error domain");
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


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database deletion lock", "[Database][C]") {
    ExpectingExceptions x;
    C4Error err;
    REQUIRE(!c4db_deleteAtPath(databasePath(), &err));
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorBusy);

    auto equivalentPath = databasePathString() + kPathSeparator;
    REQUIRE(!c4db_deleteAtPath(fleece::slice(equivalentPath), &err));
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorBusy);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Read-Only UUIDs", "[Database][C]") {
    // Make sure UUIDs are available even if the db is opened read-only when they're first accessed.
    reopenDBReadOnly();
    C4Error err;
    C4UUID publicUUID, privateUUID;
    REQUIRE(c4db_getUUIDs(db, &publicUUID, &privateUUID, &err));
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database OpenBundle", "[Database][C][!throws]") {
    auto config = *c4db_getConfig(db);

    std::string bundlePathStr = TempDir() + "cbl_core_test_bundle";
    C4Slice bundlePath = c4str(bundlePathStr.c_str());
    C4Error error;
    if (!c4db_deleteAtPath(bundlePath, &error))
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
    {
        ExpectingExceptions x;
        auto engine = config.storageEngine;
        config.storageEngine = "b0gus";
        REQUIRE(!c4db_open(bundlePath, &config, &error));
        config.storageEngine = engine;

        // Open nonexistent bundle:
        REQUIRE(!c4db_open(TEMPDIR("no_such_bundle"), &config, &error));
    }
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
    
    REQUIRE(c4db_beginTransaction(db, &error));
    REQUIRE(c4db_isInTransaction(db));
    createRev(kDocID, kRevID, kFleeceBody);
    REQUIRE(c4db_endTransaction(db, false, &error));
    REQUIRE(!c4db_isInTransaction(db));
    CHECK(c4db_getDocumentCount(db) == 0);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database CreateRawDoc", "[Database][C]") {
    const C4Slice key = c4str("key");
    const C4Slice meta = c4str("meta");
    C4Error error;
    REQUIRE(c4db_beginTransaction(db, &error));
    c4raw_put(db, c4str("test"), key, meta, kFleeceBody, &error);
    REQUIRE(c4db_endTransaction(db, true, &error));

    C4RawDocument *doc = c4raw_get(db, c4str("test"), key, &error);
    REQUIRE(doc != nullptr);
    REQUIRE(doc->key == key);
    REQUIRE(doc->meta == meta);
    REQUIRE(doc->body == kFleeceBody);
    c4raw_free(doc);

    // Nonexistent:
    REQUIRE(c4raw_get(db, c4str("test"), c4str("bogus"), &error) == (C4RawDocument*)nullptr);
    REQUIRE(error.domain == LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
    
    // Delete
    REQUIRE(c4db_beginTransaction(db, &error));
    c4raw_put(db, c4str("test"), key, kC4SliceNull, kC4SliceNull, &error);
    REQUIRE(c4db_endTransaction(db, true, &error));
    REQUIRE(c4raw_get(db, c4str("test"), key, &error) == (C4RawDocument*)nullptr);
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
    e = c4db_enumerateAllDocs(db, &options, &error);
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
        REQUIRE(doc->selectedRev.body == kFleeceBody);

        C4DocumentInfo info;
        REQUIRE(c4enum_getDocumentInfo(e, &info));
        REQUIRE(info.docID == c4str(docID));
        REQUIRE(info.flags == kDocExists);
        REQUIRE(info.revID == kRevID);
        REQUIRE(info.bodySize >= 11);
        REQUIRE(info.bodySize <= 40);   // size includes rev-tree overhead so it's not exact

        c4doc_free(doc);
        i++;
    }
    c4enum_free(e);
    REQUIRE(i == 100);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database AllDocsInfo", "[Database][C]") {
    setupAllDocs();
    C4Error error;
    C4DocEnumerator* e;

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    e = c4db_enumerateAllDocs(db, &options, &error);
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
        REQUIRE(doc.flags == (C4DocumentFlags)kDocExists);
        REQUIRE(doc.bodySize >= 11);
        REQUIRE(doc.bodySize <= 40);   // size includes rev-tree overhead so it's not exact
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

    // Descending:
    options.flags |= kC4Descending;
    e = c4db_enumerateChanges(db, 94, &options, &error);
    REQUIRE(e);
    seq = 99;
    while (nullptr != (doc = c4enum_nextDocument(e, &error))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        char docID[30];
        sprintf(docID, "doc-%03llu", (unsigned long long)seq);
        REQUIRE(doc->docID == c4str(docID));
        c4doc_free(doc);
        seq--;
    }
    c4enum_free(e);
    REQUIRE(seq == (C4SequenceNumber)94);
}

static constexpr int secs = 1000;

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Expired", "[Database][C]") {
    C4Error err;
    CHECK(c4db_nextDocExpiration(db) == 0);
    CHECK(c4db_purgeExpiredDocs(db, &err) == 0);

    C4Slice docID = C4STR("expire_me");
    createRev(docID, kRevID, kFleeceBody);
    C4Timestamp expire = c4_now() + 1*secs;
    REQUIRE(c4doc_setExpiration(db, docID, expire, &err));
    
    expire = c4_now() + 2*secs;
    // Make sure setting it to the same is also true
    REQUIRE(c4doc_setExpiration(db, docID, expire, &err));
    REQUIRE(c4doc_setExpiration(db, docID, expire, &err));
    
    C4Slice docID2 = C4STR("expire_me_too");
    createRev(docID2, kRevID, kFleeceBody);
    REQUIRE(c4doc_setExpiration(db, docID2, expire, &err));

    C4Slice docID3 = C4STR("dont_expire_me");
    createRev(docID3, kRevID, kFleeceBody);

    C4Slice docID4 = C4STR("expire_me_later");
    createRev(docID4, kRevID, kFleeceBody);
    REQUIRE(c4doc_setExpiration(db, docID4, expire + 100*secs, &err));

    REQUIRE(!c4doc_setExpiration(db, "nonexistent"_sl, expire + 50*secs, &err));
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorNotFound);

    CHECK(c4doc_getExpiration(db, docID)  == expire);
    CHECK(c4doc_getExpiration(db, docID2) == expire);
    CHECK(c4doc_getExpiration(db, docID3) == 0);
    CHECK(c4doc_getExpiration(db, docID4) == expire + 100*secs);
    CHECK(c4doc_getExpiration(db, "nonexistent"_sl) == 0);
    CHECK(c4db_nextDocExpiration(db) == expire);

    // Wait for the expiration time to pass:
    C4Log("---- Wait till expiration time...");
    sleep(2u);
    REQUIRE(c4_now() >= expire);

    C4Log("---- Purge expired docs");
    REQUIRE(c4db_purgeExpiredDocs(db, &err) == 2);

    CHECK(c4db_nextDocExpiration(db) == expire + 100*secs);

    C4Log("---- Purge expired docs (again)");
    CHECK(c4db_purgeExpiredDocs(db, &err) == 0);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database CancelExpire", "[Database][C]")
{
    C4Slice docID = C4STR("expire_me");
    createRev(docID, kRevID, kFleeceBody);
    time_t expire = c4_now() + 2*secs;
    C4Error err;
    REQUIRE(c4doc_setExpiration(db, docID, expire, &err));
    REQUIRE(c4doc_getExpiration(db, docID) == expire);
    CHECK(c4db_nextDocExpiration(db) == expire);

    REQUIRE(c4doc_setExpiration(db, docID, 0, &err));
    CHECK(c4doc_getExpiration(db, docID) == 0);
    CHECK(c4db_nextDocExpiration(db) == 0);
    CHECK(c4db_purgeExpiredDocs(db, &err) == 0);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Expired Multiple Instances", "[Database][C]") {
    // Checks that after one instance creates the 'expiration' column, other instances recognize it
    // and don't try to create it themselves.
    C4Error error;
    auto db2 = c4db_open(databasePath(), c4db_getConfig(db), &error);
    REQUIRE(db2);

    CHECK(c4db_nextDocExpiration(db) == 0);
    CHECK(c4db_nextDocExpiration(db2) == 0);

    C4Slice docID = C4STR("expire_me");
    createRev(docID, kRevID, kFleeceBody);

    uint64_t expire = c4_now() + 1*secs;
    REQUIRE(c4doc_setExpiration(db, docID, expire, &error));

    CHECK(c4db_nextDocExpiration(db2) == expire);

    c4db_free(db2);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database BlobStore", "[Database][C]")
{
    C4Error err;
    C4BlobStore *blobs = c4db_getBlobStore(db, &err);
    REQUIRE(blobs != nullptr);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Compact", "[Database][C]")
{
    C4Error err;
    C4Slice doc1ID = C4STR("doc001");
    C4Slice doc2ID = C4STR("doc002");
    C4Slice doc3ID = C4STR("doc003");
    C4Slice doc4ID = C4STR("doc004");
    string content1 = "This is the first attachment";
    string content2 = "This is the second attachment";
    string content3 = "This is the third attachment";

    vector<string> atts;
    C4BlobKey key1, key2, key3;
    {
        TransactionHelper t(db);
        atts.emplace_back(content1);
        key1 = addDocWithAttachments(doc1ID, atts, "text/plain")[0];

        atts.clear();
        atts.emplace_back(content2);
        key2 = addDocWithAttachments(doc2ID, atts, "text/plain")[0];

        addDocWithAttachments(doc4ID, atts, "text/plain");

        atts.clear();
        atts.emplace_back(content3);
        auto names = vector<string>{"att1.txt","att2.txt","att3.txt"};
        key3 = addDocWithAttachments(doc3ID, atts, "text/plain", &names)[0]; // legacy
    }
    
    C4BlobStore* store = c4db_getBlobStore(db, &err);
    REQUIRE(store);
    REQUIRE(c4db_compact(db, &err));
    REQUIRE(c4blob_getSize(store, key1) > 0);
    REQUIRE(c4blob_getSize(store, key2) > 0);
    REQUIRE(c4blob_getSize(store, key3) > 0);

    // Only reference to first blob is gone
    createRev(doc1ID, kRev2ID, kC4SliceNull, kRevDeleted);
    REQUIRE(c4db_compact(db, &err));
    REQUIRE(c4blob_getSize(store, key1) == -1);
    REQUIRE(c4blob_getSize(store, key2) > 0);
    REQUIRE(c4blob_getSize(store, key3) > 0);

    // Two references exist to the second blob, so it should still
    // exist after deleting doc002
    createRev(doc2ID, kRev2ID, kC4SliceNull, kRevDeleted);
    REQUIRE(c4db_compact(db, &err));
    REQUIRE(c4blob_getSize(store, key1) == -1);
    REQUIRE(c4blob_getSize(store, key2) > 0);
    REQUIRE(c4blob_getSize(store, key3) > 0);

    // After deleting doc4 both blobs should be gone
    createRev(doc4ID, kRev2ID, kC4SliceNull, kRevDeleted);
    REQUIRE(c4db_compact(db, &err));
    REQUIRE(c4blob_getSize(store, key2) == -1);
    REQUIRE(c4blob_getSize(store, key3) > 0);

    // Delete doc with legacy attachment, and it too will be gone
    createRev(doc3ID, kRev2ID, kC4SliceNull, kRevDeleted);
    REQUIRE(c4db_compact(db, &err));
    REQUIRE(c4blob_getSize(store, key3) == -1);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database copy", "[Database][C]") {
    C4Slice doc1ID = C4STR("doc001");
    C4Slice doc2ID = C4STR("doc002");

    createRev(doc1ID, kRevID, kFleeceBody);
    createRev(doc2ID, kRevID, kFleeceBody);
    C4SliceResult srcPath = c4db_getPath(db);
    string srcPathStr = toString((C4Slice)srcPath);
    c4slice_free(srcPath);
    string nuPath = TempDir() + "nudb.cblite2" + kPathSeparator;
    
    C4DatabaseConfig config = *c4db_getConfig(db);
    C4Error error;
    if(!c4db_deleteAtPath(c4str(nuPath.c_str()), &error)) {
        REQUIRE(error.code == 0);
    }
    
    REQUIRE(c4db_copy(c4str(srcPathStr.c_str()), c4str(nuPath.c_str()), &config, &error));
    auto nudb = c4db_open(c4str(nuPath.c_str()), &config, &error);
    REQUIRE(nudb);
    CHECK(c4db_getDocumentCount(nudb) == 2);
    REQUIRE(c4db_delete(nudb, &error));
    c4db_free(nudb);
    
    nudb = c4db_open(c4str(nuPath.c_str()), &config, &error);
    REQUIRE(nudb);
    createRev(nudb, doc1ID, kRevID, kFleeceBody);
    CHECK(c4db_getDocumentCount(nudb) == 1);
    c4db_free(nudb);
    
    string originalDest = nuPath;
    nuPath = TempDir() + "bogus" + kPathSeparator + "nunudb.cblite2" + kPathSeparator;
    {
        ExpectingExceptions x; // call to c4db_copy will internally throw an exception
        REQUIRE(!c4db_copy(c4str(srcPathStr.c_str()), c4str(nuPath.c_str()), &config, &error));
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotFound);
    }

    nudb = c4db_open(c4str(originalDest.c_str()), &config, &error);
    REQUIRE(nudb);
    CHECK(c4db_getDocumentCount(nudb) == 1);
    c4db_free(nudb);
    
    string originalSrc = srcPathStr;
    srcPathStr += string("bogus") + kPathSeparator;
    nuPath = originalDest;
    {
        ExpectingExceptions x; // call to c4db_copy will internally throw an exception
        REQUIRE(!c4db_copy(c4str(srcPathStr.c_str()), c4str(nuPath.c_str()), &config, &error));
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotFound);
    }
    
    nudb = c4db_open(c4str(originalDest.c_str()), &config, &error);
    REQUIRE(nudb);
    CHECK(c4db_getDocumentCount(nudb) == 1);
    c4db_free(nudb);

    srcPathStr = originalSrc;
    {
        ExpectingExceptions x;
        REQUIRE(!c4db_copy(c4str(srcPathStr.c_str()), c4str(nuPath.c_str()), &config, &error));
        CHECK(error.domain == POSIXDomain);
        CHECK(error.code == EEXIST);
    }

    nudb = c4db_open(c4str(nuPath.c_str()), &config, &error);
    REQUIRE(nudb);
    CHECK(c4db_getDocumentCount(nudb) == 1);
    REQUIRE(c4db_delete(nudb, &error));
    c4db_free(nudb);
}
