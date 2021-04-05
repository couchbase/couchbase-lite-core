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
#include "FilePath.hh"
#include "SecureRandomize.hh"
#include <cmath>
#include <errno.h>
#include <iostream>
#include <thread>

#include "sqlite3.h"

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

        string expectedDesc = string(expectedDomainAndType) + ", \"" + expectedMsg + "\"";
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


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database ErrorMessages", "[Database][Errors][C]") {
    alloc_slice msg = c4error_getMessage({LiteCoreDomain, 0});
    REQUIRE(msg.buf == nullptr);
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
    assertMessage(LiteCoreDomain, 15, "LiteCore CorruptData", "data is corrupted");
    assertMessage(POSIXDomain, ENOENT, "POSIX error 2", "No such file or directory");
    assertMessage(LiteCoreDomain, kC4ErrorTransactionNotClosed, "LiteCore TransactionNotClosed", "transaction not closed");
    assertMessage(SQLiteDomain, -1234, "SQLite error -1234", "unknown error (-1234)");
    assertMessage((C4ErrorDomain)666, -1234, "INVALID_DOMAIN error -1234", "invalid C4Error (unknown domain)");
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Info", "[Database][C]") {
    CHECK(c4db_exists(slice(kDatabaseName), slice(TempDir())));
    REQUIRE(c4db_getDocumentCount(db) == 0);
    REQUIRE(c4db_getLastSequence(db) == 0);
    C4UUID publicUUID, privateUUID;
    REQUIRE(c4db_getUUIDs(db, &publicUUID, &privateUUID, WITH_ERROR()));
    REQUIRE(memcmp(&publicUUID, &privateUUID, sizeof(C4UUID)) != 0);
    // Weird requirements of UUIDs according to the spec:
    REQUIRE((publicUUID.bytes[6] & 0xF0) == 0x40);
    REQUIRE((publicUUID.bytes[8] & 0xC0) == 0x80);
    REQUIRE((privateUUID.bytes[6] & 0xF0) == 0x40);
    REQUIRE((privateUUID.bytes[8] & 0xC0) == 0x80);

    // Make sure UUIDs are persistent:
    reopenDB();
    C4UUID publicUUID2, privateUUID2;
    REQUIRE(c4db_getUUIDs(db, &publicUUID2, &privateUUID2, WITH_ERROR()));
    REQUIRE(memcmp(&publicUUID, &publicUUID2, sizeof(C4UUID)) == 0);
    REQUIRE(memcmp(&privateUUID, &privateUUID2, sizeof(C4UUID)) == 0);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database deletion lock", "[Database][C]") {
    ExpectingExceptions x;
    C4Error err;
    REQUIRE(!c4db_deleteNamed(kDatabaseName, dbConfig().parentDirectory, &err));
    CHECK(err == C4Error{LiteCoreDomain, kC4ErrorBusy});

    string equivalentPath = string(slice(dbConfig().parentDirectory)) + "/";
    REQUIRE(!c4db_deleteNamed(kDatabaseName, slice(equivalentPath), &err));
    CHECK(err == C4Error{LiteCoreDomain, kC4ErrorBusy});
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Read-Only UUIDs", "[Database][C]") {
    // Make sure UUIDs are available even if the db is opened read-only when they're first accessed.
    reopenDBReadOnly();
    C4UUID publicUUID, privateUUID;
    REQUIRE(c4db_getUUIDs(db, &publicUUID, &privateUUID, WITH_ERROR()));
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database OpenNamed", "[Database][C][!throws]") {
    auto config = *c4db_getConfig2(db);

    static constexpr slice kTestBundleName = "cbl_core_test_bundle";
    C4Error error;
    if (!c4db_deleteNamed(kTestBundleName, config.parentDirectory, &error))
        REQUIRE(error.code == 0);
    auto bundle = c4db_openNamed(kTestBundleName, &config, ERROR_INFO());
    REQUIRE(bundle);
    CHECK(c4db_getName(bundle) == kTestBundleName);
    C4SliceResult path = c4db_getPath(bundle);
    CHECK(path == TEMPDIR("cbl_core_test_bundle.cblite2" kPathSeparator)); // note trailing '/'
    c4slice_free(path);
    REQUIRE(c4db_close(bundle, WITH_ERROR()));
    c4db_release(bundle);

    // Reopen without 'create' flag:
    config.flags &= ~kC4DB_Create;
    bundle = c4db_openNamed(kTestBundleName, &config, WITH_ERROR());
    REQUIRE(bundle);
    REQUIRE(c4db_close(bundle, WITH_ERROR()));
    c4db_release(bundle);

    // Reopen with wrong storage type:
    {
        ExpectingExceptions x;
        // Open nonexistent bundle:
        REQUIRE(!c4db_openNamed("no_such_bundle"_sl, &config, &error));
    }
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database OpenNamed Bad Path", "[Database][C][!throws]") {
    auto badOpen = [&](slice parentDirectory) {
        C4Error error;
        C4DatabaseConfig2 config = {};
        config.parentDirectory = parentDirectory;
        ExpectingExceptions x;
        c4::ref<C4Database> badDB = c4db_openNamed("foo"_sl, &config, ERROR_INFO(error));
        REQUIRE(badDB == nullptr);
    };

    badOpen(nullslice); // this will log an assertion failure
    badOpen("");
    badOpen("zzzzzzzzzzzzzzzz");
#ifdef WIN32
    badOpen("\\obviously\\bad\\path");
#else
    badOpen("/obviously/bad/path");
#endif
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Create Doc", "[Database][Document][C]") {
    REQUIRE(c4db_beginTransaction(db, WITH_ERROR()));
    createRev(kDocID, kRevID, kFleeceBody);
    REQUIRE(c4db_endTransaction(db, true, WITH_ERROR()));
    CHECK(c4db_getDocumentCount(db) == 1);

    C4Document* doc = REQUIRED( c4doc_get(db, kDocID, true, WITH_ERROR()) );
    CHECK(doc->docID == kDocID);
    CHECK(doc->revID == kRevID);
    CHECK(doc->sequence == 1);
    CHECK(doc->flags == kDocExists);
    CHECK(doc->selectedRev.revID == kRevID);
    CHECK(doc->selectedRev.flags == kRevLeaf);
    CHECK(doc->selectedRev.sequence == 1);
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Transaction", "[Database][Document][C]") {
    REQUIRE(c4db_getDocumentCount(db) == (C4SequenceNumber)0);
    REQUIRE(!c4db_isInTransaction(db));
    REQUIRE(c4db_beginTransaction(db, WITH_ERROR()));
    REQUIRE(c4db_isInTransaction(db));
    REQUIRE(c4db_beginTransaction(db, WITH_ERROR()));
    REQUIRE(c4db_isInTransaction(db));
    REQUIRE(c4db_endTransaction(db, true, WITH_ERROR()));
    REQUIRE(c4db_isInTransaction(db));
    REQUIRE(c4db_endTransaction(db, true, WITH_ERROR()));
    REQUIRE(!c4db_isInTransaction(db));
    
    REQUIRE(c4db_beginTransaction(db, WITH_ERROR()));
    REQUIRE(c4db_isInTransaction(db));
    createRev(kDocID, kRevID, kFleeceBody);
    REQUIRE(c4db_endTransaction(db, false, WITH_ERROR()));
    REQUIRE(!c4db_isInTransaction(db));
    CHECK(c4db_getDocumentCount(db) == 0);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database CreateRawDoc", "[Database][Document][C]") {
    const C4Slice key = c4str("key");
    const C4Slice meta = c4str("meta");
    C4Error error;
    REQUIRE(c4db_beginTransaction(db, WITH_ERROR()));
    REQUIRE(c4raw_put(db, c4str("test"), key, meta, kFleeceBody, WITH_ERROR()));
    REQUIRE(c4db_endTransaction(db, true, WITH_ERROR()));

    C4RawDocument *doc = REQUIRED(c4raw_get(db, c4str("test"), key, WITH_ERROR()));
    REQUIRE(doc->key == key);
    REQUIRE(doc->meta == meta);
    REQUIRE(doc->body == kFleeceBody);
    c4raw_free(doc);

    // Nonexistent:
    REQUIRE(c4raw_get(db, c4str("test"), c4str("bogus"), &error) == nullptr);
    REQUIRE(error == C4Error{LiteCoreDomain, kC4ErrorNotFound});
    
    // Delete
    REQUIRE(c4db_beginTransaction(db, WITH_ERROR()));
    REQUIRE(c4raw_put(db, c4str("test"), key, kC4SliceNull, kC4SliceNull, WITH_ERROR()));
    REQUIRE(c4db_endTransaction(db, true, WITH_ERROR()));
    REQUIRE(c4raw_get(db, c4str("test"), key, &error) == (C4RawDocument*)nullptr);
    REQUIRE(error.domain == LiteCoreDomain);
    REQUIRE(error.code == (int)kC4ErrorNotFound);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Enumerator", "[Database][Document][Enumerator][C]") {
    setupAllDocs();
    C4Error error;
    C4DocEnumerator* e;

    REQUIRE(c4db_getDocumentCount(db) == 99);

    // No start or end ID:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    options.flags &= ~kC4IncludeBodies;
    e = REQUIRED(c4db_enumerateAllDocs(db, &options, WITH_ERROR()));
    char docID[20];
    int i = 1;
    while (c4enum_next(e, &error)) {
        auto doc = c4enum_getDocument(e, ERROR_INFO());
        REQUIRE(doc);
        sprintf(docID, "doc-%03d", i);
        CHECK(doc->docID == c4str(docID));
        CHECK(doc->revID == kRevID);
        CHECK(doc->selectedRev.revID == kRevID);
        CHECK(doc->selectedRev.sequence == (C4SequenceNumber)i);
        CHECK(c4doc_getProperties(doc) == nullptr);
        // Doc was loaded without its body, but it should load on demand:
        CHECK(c4doc_loadRevisionBody(doc, WITH_ERROR())); // have to explicitly load the body
        CHECK(docBodyEquals(doc, kFleeceBody));

        C4DocumentInfo info;
        REQUIRE(c4enum_getDocumentInfo(e, &info));
        CHECK(info.docID == c4str(docID));
        CHECK(info.flags == kDocExists);
        CHECK(info.revID == kRevID);
        CHECK(info.bodySize == kFleeceBody.size);

        c4doc_release(doc);
        i++;
    }
    c4enum_free(e);
    CHECK(error == C4Error{});
    CHECK(i == 100);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Enumerator With Info", "[Database][Enumerator][C]") {
    setupAllDocs();
    C4Error error;
    C4DocEnumerator* e;

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    e = c4db_enumerateAllDocs(db, &options, ERROR_INFO());
    CHECK(e);
    int i = 1;
    while(c4enum_next(e, &error)) {
        C4DocumentInfo info;
        REQUIRE(c4enum_getDocumentInfo(e, &info));
        char docID[20];
        sprintf(docID, "doc-%03d", i);
        CHECK(info.docID == c4str(docID));
        CHECK(info.revID == kRevID);
        CHECK(info.sequence == (uint64_t)i);
        CHECK(info.flags == (C4DocumentFlags)kDocExists);
        CHECK(info.bodySize == kFleeceBody.size);
        if (isRevTrees())
            CHECK(info.metaSize > 0);       // (VV doesn't use `extra` until remote revs are added)
        i++;
    }
    c4enum_free(e);
    CHECK(error == C4Error{});
    CHECK(i == 100);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Enumerator With History", "[Database][Enumerator][C]") {
    if (isRevTrees())
        return;

    C4DocEnumerator* e;

    createRev("doc-100"_sl, "1@*"_sl, kFleeceBody);
    createRev("doc-100"_sl, "1@d00d"_sl, kFleeceBody);

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    e = c4db_enumerateAllDocs(db, &options, ERROR_INFO());
    REQUIRE(e);
    REQUIRE(c4enum_next(e, WITH_ERROR()));
    C4DocumentInfo info;
    REQUIRE(c4enum_getDocumentInfo(e, &info));
    CHECK(info.docID == "doc-100"_sl);
    CHECK(info.revID == "1@d00d"_sl);       // Latest version only
    c4enum_free(e);

    options.flags |= kC4IncludeRevHistory;
    e = c4db_enumerateAllDocs(db, &options, ERROR_INFO());
    REQUIRE(e);
    REQUIRE(c4enum_next(e, WITH_ERROR()));
    REQUIRE(c4enum_getDocumentInfo(e, &info));
    CHECK(info.docID == "doc-100"_sl);
    CHECK(info.revID == "1@d00d,1@*"_sl);       // Entire version vector
    c4enum_free(e);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Changes", "[Database][Enumerator][C]") {
    createNumberedDocs(99);

    C4Error error;
    C4DocEnumerator* e;
    C4Document* doc;

    // Since start:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    options.flags &= ~kC4IncludeBodies;
    e = c4db_enumerateChanges(db, 0, &options, ERROR_INFO());
    REQUIRE(e);
    C4SequenceNumber seq = 1;
    while (nullptr != (doc = c4enum_nextDocument(e, &error))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        char docID[30];
        sprintf(docID, "doc-%03llu", (unsigned long long)seq);
        REQUIRE(doc->docID == c4str(docID));
        c4doc_release(doc);
        seq++;
    }
    CHECK(error == C4Error{});
    c4enum_free(e);

    // Since 6:
    e = c4db_enumerateChanges(db, 6, &options, ERROR_INFO());
    REQUIRE(e);
    seq = 7;
    while (nullptr != (doc = c4enum_nextDocument(e, &error))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        char docID[30];
        sprintf(docID, "doc-%03llu", (unsigned long long)seq);
        REQUIRE(doc->docID == c4str(docID));
        c4doc_release(doc);
        seq++;
    }
    c4enum_free(e);
    CHECK(error == C4Error{});
    REQUIRE(seq == (C4SequenceNumber)100);

    // Descending:
    options.flags |= kC4Descending;
    e = c4db_enumerateChanges(db, 94, &options, ERROR_INFO());
    REQUIRE(e);
    seq = 99;
    while (nullptr != (doc = c4enum_nextDocument(e, &error))) {
        REQUIRE(doc->selectedRev.sequence == seq);
        char docID[30];
        sprintf(docID, "doc-%03llu", (unsigned long long)seq);
        REQUIRE(doc->docID == c4str(docID));
        c4doc_release(doc);
        seq--;
    }
    c4enum_free(e);
    CHECK(error == C4Error{});
    REQUIRE(seq == (C4SequenceNumber)94);
}


#pragma mark - DOCUMENT EXPIRATION:


static constexpr int secs = 1000;
static constexpr int ms = 1;

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Expired", "[Database][C][Expiration]") {
    C4Error err;
    CHECK(c4db_nextDocExpiration(db) == 0);
    CHECK(c4db_purgeExpiredDocs(db, WITH_ERROR()) == 0);
    CHECK(!c4db_mayHaveExpiration(db));

    C4Slice docID = C4STR("expire_me");
    createRev(docID, kRevID, kFleeceBody);
    C4Timestamp expire = c4_now() + 1*secs;
    REQUIRE(c4doc_setExpiration(db, docID, expire, WITH_ERROR()));

    CHECK(c4db_mayHaveExpiration(db));

    expire = c4_now() + 2*secs;
    // Make sure setting it to the same is also true
    REQUIRE(c4doc_setExpiration(db, docID, expire, WITH_ERROR()));
    REQUIRE(c4doc_setExpiration(db, docID, expire, WITH_ERROR()));
    
    C4Slice docID2 = C4STR("expire_me_too");
    createRev(docID2, kRevID, kFleeceBody);
    REQUIRE(c4doc_setExpiration(db, docID2, expire, WITH_ERROR()));

    C4Slice docID3 = C4STR("dont_expire_me");
    createRev(docID3, kRevID, kFleeceBody);

    C4Slice docID4 = C4STR("expire_me_later");
    createRev(docID4, kRevID, kFleeceBody);
    REQUIRE(c4doc_setExpiration(db, docID4, expire + 100*secs, WITH_ERROR()));

    REQUIRE(!c4doc_setExpiration(db, "nonexistent"_sl, expire + 50*secs, &err));
    CHECK(err == C4Error{LiteCoreDomain, kC4ErrorNotFound});

    CHECK(c4doc_getExpiration(db, docID, nullptr)  == expire);
    CHECK(c4doc_getExpiration(db, docID2, nullptr) == expire);
    CHECK(c4doc_getExpiration(db, docID3, nullptr) == 0);
    CHECK(c4doc_getExpiration(db, docID4, nullptr) == expire + 100*secs);
    CHECK(c4doc_getExpiration(db, "nonexistent"_sl, nullptr) == 0);

    CHECK(c4db_nextDocExpiration(db) == expire);

    // Wait for the expiration time to pass:
    C4Log("---- Wait till expiration time...");
    this_thread::sleep_for(2000ms);
    REQUIRE(c4_now() >= expire);

    C4Log("---- Purge expired docs");
    REQUIRE(c4db_purgeExpiredDocs(db, WITH_ERROR()) == 2);

    CHECK(c4db_nextDocExpiration(db) == expire + 100*secs);

    C4Log("---- Purge expired docs (again)");
    CHECK(c4db_purgeExpiredDocs(db, WITH_ERROR()) == 0);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Auto-Expiration", "[Database][C][Expiration]")
{
    CHECK(!c4db_mayHaveExpiration(db));
    c4db_startHousekeeping(db);

    createRev("expire_me"_sl, kRevID, kFleeceBody);
    C4Timestamp expire = c4_now() + 10000*ms;
    C4Error err;
    REQUIRE(c4doc_setExpiration(db, "expire_me"_sl, expire, WITH_ERROR()));
    CHECK(c4db_mayHaveExpiration(db));

    createRev("expire_me_first"_sl, kRevID, kFleeceBody);
    expire = c4_now() + 1500*ms;
    REQUIRE(c4doc_setExpiration(db, "expire_me_first"_sl, expire, WITH_ERROR()));

    auto docExists = [&] {
        auto doc = c4::make_ref(c4doc_get(db, "expire_me_first"_sl, true, &err));
        return doc != nullptr;
    };

    // Wait for the expiration time to pass:
    C4Log("---- Wait till expiration time...");
    this_thread::sleep_for(1500ms);
    CHECK_BEFORE(15s, ! docExists());
    CHECK(err == C4Error{LiteCoreDomain, kC4ErrorNotFound});
    C4Log("---- Done...");
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Auto-Expiration After Reopen", "[Database][C][Expiration]")
{
    CHECK(!c4db_mayHaveExpiration(db));
    createRev("expire_me_first"_sl, kRevID, kFleeceBody);
    auto expire = c4_now() + 1500*ms;
    C4Error err;
    REQUIRE(c4doc_setExpiration(db, "expire_me_first"_sl, expire, WITH_ERROR()));
    CHECK(c4db_mayHaveExpiration(db));

    C4Log("---- Reopening DB...");
    reopenDB();
    CHECK(c4db_mayHaveExpiration(db));
    c4db_startHousekeeping(db);

    auto docExists = [&] {
        auto doc = c4::make_ref(c4doc_get(db, "expire_me_first"_sl, true, &err));
        return doc != nullptr;
    };

    // Wait for the expiration time to pass:
    C4Log("---- Wait till expiration time...");
    this_thread::sleep_for(1500ms);
    CHECK_BEFORE(10s, ! docExists());
    CHECK(err.domain == LiteCoreDomain);
    CHECK(err.code == kC4ErrorNotFound);
    C4Log("---- Done...");
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database CancelExpire", "[Database][C][Expiration]")
{
    C4Slice docID = C4STR("expire_me");
    createRev(docID, kRevID, kFleeceBody);
    C4Timestamp expire = c4_now() + 2*secs;
    REQUIRE(c4doc_setExpiration(db, docID, expire, WITH_ERROR()));
    REQUIRE(c4doc_getExpiration(db, docID, nullptr) == expire);
    CHECK(c4db_nextDocExpiration(db) == expire);

    REQUIRE(c4doc_setExpiration(db, docID, 0, WITH_ERROR()));
    CHECK(c4doc_getExpiration(db, docID, nullptr) == 0);
    CHECK(c4db_nextDocExpiration(db) == 0);
    CHECK(c4db_purgeExpiredDocs(db, WITH_ERROR()) == 0);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Expired Multiple Instances", "[Database][C][Expiration]") {
    // Checks that after one instance creates the 'expiration' column, other instances recognize it
    // and don't try to create it themselves.
    auto db2 = c4db_openAgain(db, ERROR_INFO());
    REQUIRE(db2);

    CHECK(c4db_nextDocExpiration(db) == 0);
    CHECK(c4db_nextDocExpiration(db2) == 0);

    C4Slice docID = C4STR("expire_me");
    createRev(docID, kRevID, kFleeceBody);

    C4Timestamp expire = c4_now() + 1*secs;
    REQUIRE(c4doc_setExpiration(db, docID, expire, WITH_ERROR()));

    CHECK(c4db_nextDocExpiration(db2) == expire);

    c4db_release(db2);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database BackgroundDB torture test", "[Database][C][Expiration]")
{
    // Test for random crashers/race conditions closing a database with a BackgroundDB+Housekeeper.
    // See CBL-980, CBL-984.

    // Suppress logging so test can run faster:
    auto oldLevel = c4log_getLevel(kC4DatabaseLog);
    c4log_setLevel(kC4DatabaseLog, kC4LogWarning);

    auto stopAt = c4_now() + 5*secs;
    do {
        C4LogToAt(kC4DatabaseLog, kC4LogInfo, "---- start housekeeping ---");
        c4db_startHousekeeping(db);

        char docID[50];
        c4doc_generateID(docID, sizeof(docID));
        createRev(slice(docID), kRevID, kFleeceBody);
        C4Timestamp expire = c4_now() + 2*secs;
        REQUIRE(c4doc_setExpiration(db, slice(docID), expire, nullptr));

        int n = litecore::RandomNumber(1000);
        this_thread::sleep_for(chrono::microseconds(n));
        C4LogToAt(kC4DatabaseLog, kC4LogInfo, "---- close & reopen db ---");
        reopenDB();
    } while (c4_now() < stopAt);

    c4log_setLevel(kC4DatabaseLog, oldLevel);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database BlobStore", "[Database][C][Blob]")
{
    C4BlobStore *blobs = c4db_getBlobStore(db, ERROR_INFO());
    REQUIRE(blobs != nullptr);
}

N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Compact", "[Database][C][Blob]")
{
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
    
    C4BlobStore* store = c4db_getBlobStore(db, ERROR_INFO());
    REQUIRE(store);
    REQUIRE(c4db_maintenance(db, kC4Compact, WITH_ERROR()));
    REQUIRE(c4blob_getSize(store, key1) > 0);
    REQUIRE(c4blob_getSize(store, key2) > 0);
    REQUIRE(c4blob_getSize(store, key3) > 0);

    // Only reference to first blob is gone
    createRev(doc1ID, kRev2ID, kC4SliceNull, kRevDeleted);
    REQUIRE(c4db_maintenance(db, kC4Compact, WITH_ERROR()));
    REQUIRE(c4blob_getSize(store, key1) == -1);
    REQUIRE(c4blob_getSize(store, key2) > 0);
    REQUIRE(c4blob_getSize(store, key3) > 0);

    // Two references exist to the second blob, so it should still
    // exist after deleting doc002
    createRev(doc2ID, kRev2ID, kC4SliceNull, kRevDeleted);
    REQUIRE(c4db_maintenance(db, kC4Compact, WITH_ERROR()));
    REQUIRE(c4blob_getSize(store, key1) == -1);
    REQUIRE(c4blob_getSize(store, key2) > 0);
    REQUIRE(c4blob_getSize(store, key3) > 0);

    // After deleting doc4 both blobs should be gone
    createRev(doc4ID, kRev2ID, kC4SliceNull, kRevDeleted);
    REQUIRE(c4db_maintenance(db, kC4Compact, WITH_ERROR()));
    REQUIRE(c4blob_getSize(store, key2) == -1);
    REQUIRE(c4blob_getSize(store, key3) > 0);

    // Delete doc with legacy attachment, and it too will be gone
    createRev(doc3ID, kRev2ID, kC4SliceNull, kRevDeleted);
    REQUIRE(c4db_maintenance(db, kC4Compact, WITH_ERROR()));
    REQUIRE(c4blob_getSize(store, key3) == -1);

    // Try an integrity check too
    REQUIRE(c4db_maintenance(db, kC4IntegrityCheck, WITH_ERROR()));
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database copy", "[Database][C]") {
    static constexpr slice kNuName = "nudb";

    C4Slice doc1ID = C4STR("doc001");
    C4Slice doc2ID = C4STR("doc002");

    createRev(doc1ID, kRevID, kFleeceBody);
    createRev(doc2ID, kRevID, kFleeceBody);
    string srcPathStr = toString(c4db_getPath(db));

    C4DatabaseConfig2 config = *c4db_getConfig2(db);
    string nuPath = string(slice(config.parentDirectory)) + string(kNuName) + ".cblite2" + kPathSeparator;
    C4Error error;
    if(!c4db_deleteNamed(kNuName, config.parentDirectory, &error)) {
        REQUIRE(error.code == 0);
    }
    
    REQUIRE(c4db_copyNamed(c4str(srcPathStr.c_str()), kNuName, &config, WITH_ERROR()));
    auto nudb = c4db_openNamed(kNuName, &config, ERROR_INFO());
    REQUIRE(nudb);
    CHECK(c4db_getDocumentCount(nudb) == 2);
    REQUIRE(c4db_delete(nudb, WITH_ERROR()));
    c4db_release(nudb);
    
    nudb = c4db_openNamed(kNuName, &config, ERROR_INFO());
    REQUIRE(nudb);
    createRev(nudb, doc1ID, kRevID, kFleeceBody);
    CHECK(c4db_getDocumentCount(nudb) == 1);
    c4db_release(nudb);
    
    {
        string bogusPath = TempDir() + "bogus" + kPathSeparator + "bogus";
        C4DatabaseConfig2 bogusConfig = config;
        bogusConfig.parentDirectory = slice(bogusPath);
        ExpectingExceptions x; // call to c4db_copy will internally throw an exception
        REQUIRE(!c4db_copyNamed(c4str(srcPathStr.c_str()), kNuName, &bogusConfig, &error));
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotFound);
    }

    nudb = c4db_openNamed(kNuName, &config, ERROR_INFO());
    REQUIRE(nudb);
    CHECK(c4db_getDocumentCount(nudb) == 1);
    c4db_release(nudb);
    
    {
        string bogusSrcPathStr = srcPathStr + "bogus" + kPathSeparator;
        ExpectingExceptions x; // call to c4db_copy will internally throw an exception
        REQUIRE(!c4db_copyNamed(c4str(bogusSrcPathStr.c_str()), kNuName, &config, &error));
        CHECK(error.domain == LiteCoreDomain);
        CHECK(error.code == kC4ErrorNotFound);
    }
    
    nudb = c4db_openNamed(kNuName, &config, ERROR_INFO());
    REQUIRE(nudb);
    CHECK(c4db_getDocumentCount(nudb) == 1);
    c4db_release(nudb);

    {
        ExpectingExceptions x;
        REQUIRE(!c4db_copyNamed(c4str(srcPathStr.c_str()), kNuName, &config, &error));
        CHECK(error.domain == POSIXDomain);
        CHECK(error.code == EEXIST);
    }

    nudb = c4db_openNamed(kNuName, &config, ERROR_INFO());
    REQUIRE(nudb);
    CHECK(c4db_getDocumentCount(nudb) == 1);
    REQUIRE(c4db_delete(nudb, WITH_ERROR()));
    c4db_release(nudb);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Config2 And ExtraInfo", "[Database][C]") {
    C4DatabaseConfig2 config = {};
    config.parentDirectory = slice(TempDir());
    config.flags = kC4DB_Create;
    const string db2Name = string(kDatabaseName) + "_2";
    C4Error error;

    c4db_deleteNamed(slice(db2Name), config.parentDirectory, &error);
    REQUIRE(error.code == 0);
    CHECK(!c4db_exists(slice(db2Name), config.parentDirectory));
    C4Database *db2 = c4db_openNamed(slice(db2Name), &config, ERROR_INFO());
    REQUIRE(db2);
    alloc_slice db2Path = c4db_getPath(db2);
    CHECK(c4db_exists(slice(db2Name), config.parentDirectory));

    C4ExtraInfo xtra = {};
    xtra.pointer = this;
    static void* sExpectedPointer;
    static bool sXtraDestructed;
    sExpectedPointer = this;
    sXtraDestructed = false;
    xtra.destructor = [](void *ptr) {
        REQUIRE(ptr == sExpectedPointer);
        REQUIRE(!sXtraDestructed);
        sXtraDestructed = true;
    };
    c4db_setExtraInfo(db2, xtra);
    CHECK(!sXtraDestructed);
    CHECK(c4db_getExtraInfo(db2).pointer == this);
    CHECK(c4db_close(db2, WITH_ERROR()));
    CHECK(!sXtraDestructed);
    c4db_release(db2);
    CHECK(sXtraDestructed);

    const string copiedDBName = string(kDatabaseName) + "_copy";
    c4db_deleteNamed(slice(copiedDBName), config.parentDirectory, ERROR_INFO());
    REQUIRE(error.code == 0);
    REQUIRE(c4db_copyNamed(db2Path, slice(copiedDBName), &config, WITH_ERROR()));
    CHECK(c4db_exists(slice(copiedDBName), config.parentDirectory));
    REQUIRE(c4db_deleteNamed(slice(copiedDBName), config.parentDirectory, WITH_ERROR()));

    REQUIRE(c4db_deleteNamed(slice(db2Name), config.parentDirectory, WITH_ERROR()));
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Reject invalid top-level keys", "[Database][C]") {
    C4Slice badKeys[] = { C4STR("_id"), C4STR("_rev"), C4STR("_deleted") };
    ExpectingExceptions ee;
    for(const auto key : badKeys) {
        TransactionHelper t(db);
        SharedEncoder enc(c4db_getSharedFleeceEncoder(db));
        enc.beginDict();
        enc.writeKey(key);
        enc.writeInt(1234);
        enc.endDict();
        fleece::alloc_slice fleeceBody = enc.finish();

        C4Slice history[1] = { C4STR("1-abcd") };
        C4DocPutRequest rq = {};
        rq.existingRevision = true;
        rq.docID = C4STR("test");
        rq.history = history;
        rq.historyCount = 1;
        rq.body = fleeceBody;
        rq.save = true;
        C4Error error;
        auto doc = c4doc_put(db, &rq, nullptr, &error);
        CHECK(doc == nullptr);
        CHECK(error == C4Error{LiteCoreDomain, kC4ErrorCorruptRevisionData});
    }
}


#pragma mark - SCHEMA UPGRADES


static const string kVersionedFixturesSubDir = "db_versions/";


// This isn't normally run. It creates a new database to check into C/tests/data/db_versions/.
N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Create Upgrade Fixture", "[.Maintenance]") {
    {
        TransactionHelper t(db);
        char docID[20], json[100];
        for (unsigned i = 1; i <= 100; i++) {
            sprintf(docID, "doc-%03u", i);
            sprintf(json, R"({"n":%d, "even":%s})", i, (i%2 ? "false" : "true"));
            createFleeceRev(db, slice(docID), kRevID, slice(json), (i <= 50 ? 0 : kRevDeleted));
        }
        // TODO: Create some blobs too
    }
    alloc_slice path = c4db_getPath(db);
    string filename = "NEW_UPGRADE_FIXTURE.cblite2";
    if (c4db_getConfig2(db)->encryptionKey.algorithm != kC4EncryptionNone)
        filename = "NEW_ENCRYPTED_UPGRADE_FIXTURE.cblite2";

    closeDB();

    litecore::FilePath fixturePath(C4DatabaseTest::sFixturesDir + kVersionedFixturesSubDir + filename, "");
    litecore::FilePath(string(path), "").moveToReplacingDir(fixturePath, false);
    C4Log("New fixture is at %s", string(fixturePath).c_str());
}


static void testOpeningOlderDBFixture(const string & dbPath,
                                      C4DatabaseFlags withFlags,
                                      int expectedErrorCode =0)
{
    C4Log("---- Opening copy of db %s with flags 0x%x", dbPath.c_str(), withFlags);
    C4DatabaseConfig2 config = {slice(TempDir()), withFlags};
    C4Error error;
    C4Database *db;
    auto name = C4Test::copyFixtureDB(kVersionedFixturesSubDir + dbPath);

    if (expectedErrorCode == 0) {
        db = c4db_openNamed(name, &config, ERROR_INFO());
        REQUIRE(db);
    } else {
        ExpectingExceptions x;
        db = c4db_openNamed(name, &config, &error);
        REQUIRE(!db);
        REQUIRE(error.domain == LiteCoreDomain);
        REQUIRE(error.code == expectedErrorCode);
        return;
    }

    // Verify getting documents by ID:
    char docID[20];
    for (unsigned i = 1; i <= 100; i++) {
        sprintf(docID, "doc-%03u", i);
        INFO("Checking docID " << docID);
        C4Document *doc = c4doc_get(db, slice(docID), true, ERROR_INFO());
        REQUIRE(doc);
        if (i <= 50)
            CHECK((doc->flags & kRevDeleted) == 0);
        else
            CHECK((doc->flags & kRevDeleted) != 0);
        // TODO: Verify the doc contents too
        c4doc_release(doc);
    }

    // Verify enumerating documents:
    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    options.flags |= kC4IncludeDeleted;
    C4DocEnumerator *e = c4db_enumerateAllDocs(db, &options, ERROR_INFO());
    REQUIRE(e);
    unsigned i = 1;
    while (c4enum_next(e, ERROR_INFO(&error))) {
        INFO("Checking enumeration #" << i);
        sprintf(docID, "doc-%03u", i);
        C4DocumentInfo info;
        REQUIRE(c4enum_getDocumentInfo(e, &info));
        CHECK(slice(info.docID) == slice(docID));
        ++i;
    }
    CHECK(error == C4Error{});
    CHECK(i == 101);

    CHECK(c4db_delete(db, WITH_ERROR()));
    c4db_release(db);
}


TEST_CASE("Database Upgrade From 2.7 to New Rev-Trees", "[Database][Upgrade][C]") {
    testOpeningOlderDBFixture("upgrade_2.7.cblite2", 0);
// In 3.0 it's no longer possible to open 2.7 databases without upgrading
//    testOpeningOlderDBFixture("upgrade_2.7.cblite2", kC4DB_NoUpgrade);
//    testOpeningOlderDBFixture("upgrade_2.7.cblite2", kC4DB_ReadOnly);
}


TEST_CASE("Database Upgrade From 2.7 to Version Vectors", "[Database][Upgrade][C]") {
    testOpeningOlderDBFixture("upgrade_2.7.cblite2", kC4DB_VersionVectors);
}


static void setRemoteRev(C4Database *db, slice docID, slice revID, C4RemoteID remote) {
    C4Document *doc = c4db_getDoc(db, docID, true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);
    REQUIRE(c4doc_setRemoteAncestor(doc, remote, revID, WITH_ERROR()));
    REQUIRE(c4doc_save(doc, 0, WITH_ERROR()));
    c4doc_release(doc);
}


N_WAY_TEST_CASE_METHOD(C4DatabaseTest, "Database Upgrade To Version Vectors", "[Database][Upgrade][C]") {
    if (!isRevTrees())
        return;

    {
        // Initially populate a v3 rev-tree based database:
        TransactionHelper t(db);
        createNumberedDocs(5);
        // Add a deleted doc to make sure it's skipped by default:
        createRev(c4str("doc-DEL"), kRevID, kC4SliceNull, kRevDeleted);

        // Add a 2nd revision to doc 1:
        createFleeceRev(db, "doc-001"_sl, nullslice, slice(json5("{doc:'one',rev:'two'}")));

        // Add a 2nd rev, synced with remote, to doc 2:
        createFleeceRev(db, "doc-002"_sl, kRev2ID, slice(json5("{doc:'two',rev:'two'}")));
        createFleeceRev(db, "doc-002"_sl, kRev3ID, slice(json5("{doc:'two',rev:'three'}")));
        setRemoteRev(db, "doc-002"_sl, kRev3ID, 1);

        // Add a 2nd rev, and rev 1 synced with remote, to doc 3:
        createFleeceRev(db, "doc-003"_sl, kRev2ID, slice(json5("{doc:'three',rev:'two'}")), kRevKeepBody);
        setRemoteRev(db, "doc-003"_sl, kRev2ID, 1);
        createFleeceRev(db, "doc-003"_sl, kRev3ID, slice(json5("{doc:'three',rev:'three'}")));

        // Add a conflict to doc 4:
        createFleeceRev(db, "doc-004"_sl, kRev2ID, slice(json5("{doc:'four',rev:'two'}")));
        createFleeceRev(db, "doc-004"_sl, kRev3ID, slice(json5("{doc:'four',rev:'three'}")));
        createConflictingRev(db, "doc-004"_sl, kRev2ID, "3-cc"_sl);
        setRemoteRev(db, "doc-004"_sl, "3-cc"_sl, 1);
    }

    // Reopen database, upgrading to version vectors:
    C4DatabaseConfig2 config = dbConfig();
    config.flags |= kC4DB_VersionVectors;
    closeDB();
    C4Log("---- Reopening db with version vectors ---");
    db = c4db_openNamed(kDatabaseName, &config, ERROR_INFO());
    REQUIRE(db);

    // Note: The revID/version checks below hardcode the "legacy source ID", currently 0x7777777.
    // If/when that's changed (in Database+Upgrade.cc), those checks will break.

    // Check doc 1:
    C4Document *doc;
    doc = c4db_getDoc(db, "doc-001"_sl, true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);
    CHECK(slice(doc->revID) == "2@*");
    alloc_slice versionVector(c4doc_getRevisionHistory(doc, 0, nullptr, 0));
    CHECK(versionVector == "2@*");
    CHECK(doc->sequence == 7);
    CHECK(Dict(c4doc_getProperties(doc)).toJSONString() == R"({"doc":"one","rev":"two"})");
    c4doc_release(doc);

    // Check doc 2:
    doc = c4db_getDoc(db, "doc-002"_sl, true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);
    CHECK(slice(doc->revID) == "3@7777777");
    versionVector = c4doc_getRevisionHistory(doc, 0, nullptr, 0);
    CHECK(versionVector == "3@7777777");
    CHECK(doc->sequence == 9);
    CHECK(Dict(c4doc_getProperties(doc)).toJSONString() == R"({"doc":"two","rev":"three"})");
    alloc_slice remoteVers = c4doc_getRemoteAncestor(doc, 1);
    CHECK(remoteVers == "3@7777777");
    CHECK(c4doc_selectRevision(doc, remoteVers, true, WITH_ERROR()));
    CHECK(Dict(c4doc_getProperties(doc)).toJSONString() == R"({"doc":"two","rev":"three"})");
    c4doc_release(doc);

    // Check doc 3:
    doc = c4db_getDoc(db, "doc-003"_sl, true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);
    CHECK(slice(doc->revID) == "1@*");
    versionVector = c4doc_getRevisionHistory(doc, 0, nullptr, 0);
    CHECK(versionVector == "1@*,2@7777777");
    CHECK(doc->sequence == 11);
    CHECK(Dict(c4doc_getProperties(doc)).toJSONString() == R"({"doc":"three","rev":"three"})");
    remoteVers = c4doc_getRemoteAncestor(doc, 1);
    CHECK(remoteVers == "2@7777777");
    CHECK(c4doc_selectRevision(doc, remoteVers, true, WITH_ERROR()));
    CHECK(Dict(c4doc_getProperties(doc)).toJSONString() == R"({"doc":"three","rev":"two"})");
    c4doc_release(doc);

    // Check doc 4:
    doc = c4db_getDoc(db, "doc-004"_sl, true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);
    CHECK(slice(doc->revID) == "1@*");
    versionVector = c4doc_getRevisionHistory(doc, 0, nullptr, 0);
    CHECK(versionVector == "1@*,2@7777777");
    CHECK(doc->sequence == 14);
    CHECK(doc->flags == (kDocConflicted | kDocExists));
    CHECK(Dict(c4doc_getProperties(doc)).toJSONString() == R"({"doc":"four","rev":"three"})");
    remoteVers = c4doc_getRemoteAncestor(doc, 1);
    CHECK(remoteVers == "3@7777777");
    REQUIRE(c4doc_selectRevision(doc, remoteVers, true, WITH_ERROR()));
    versionVector = c4doc_getRevisionHistory(doc, 0, nullptr, 0);
    CHECK(versionVector == "3@7777777");
    CHECK(c4doc_selectRevision(doc, remoteVers, true, WITH_ERROR()));
    CHECK(Dict(c4doc_getProperties(doc)).toJSONString() == R"({"ans*wer":42})");
    c4doc_release(doc);

    // Check deleted doc:
    doc = c4db_getDoc(db, "doc-DEL"_sl, true, kDocGetAll, ERROR_INFO());
    REQUIRE(doc);
    CHECK(slice(doc->revID) == "1@*");
    versionVector = c4doc_getRevisionHistory(doc, 0, nullptr, 0);
    CHECK(versionVector == "1@*");
    CHECK(doc->sequence == 6);
    CHECK(doc->flags == (kDocDeleted | kDocExists));
    c4doc_release(doc);
}
