//
// ReplicatorWalrusTest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Base64.hh"
#include "ReplicatorAPITest.hh"
#include "CertHelper.hh"
#include "c4BlobStore.h"
#include "c4Document+Fleece.h"
#include "c4DocEnumerator.h"
#include "c4Index.h"
#include "c4Query.h"
#include "ReplicatorSGTest.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "SecureRandomize.hh"
#include "fleece/Fleece.hh"
#include <cstdlib>

#ifndef _MSC_VER
#include <unistd.h>
#endif

using namespace fleece;

constexpr size_t kDocBufSize = 40;


/* REAL-REPLICATOR (SYNC GATEWAY) TESTS

 The tests below are tagged [.SyncServerWalrus] to keep them from running during normal testing.
 Instead, they have to be invoked manually via the Catch command-line option `[.SyncServerWalrus]`.
 This is because they require that an external replication server is running.

 The default URL the tests connect to is blip://localhost:4984/scratch/, but this can be
 overridden by setting environment vars listed below.

 WARNING: The tests will erase the database named by REMOTE_DB (via the SG REST API.)

 Some tests connect to other databases by setting `_sg.remoteDBName`. These have fixed contents.
 The directory Replicator/tests/data/ contains Sync Gateway config files and Walrus data files,
 so if you `cd` to that directory and enter `sync_gateway config.json` you should be good to go.
 (For more details, see the README.md file in that directory.)

 Environment variables to configure the connection:
     REMOTE_TLS (or REMOTE_SSL)     If defined, use TLS
     REMOTE_HOST                    Hostname to connect to (default: localhost)
     REMOTE_PORT                    Port number (default: 4984)
     REMOTE_DB                      Database name (default: "scratch")
     REMOTE_PROXY                   HTTP proxy URL to use (default: none)
     USE_CLIENT_CERT                If defined, send a TLS client cert [EE only!]
 */

// Tests in this file work with SGW v3.0 in the walrus mode as instructed in the above.
// Except for few tests that involves pre-populated , most tests also work with SGW v3.1, non-walrus mode, by uncommenting
// the folllowing define,
//
//#define NOT_WALRUS
//
// The main differences between the walrus SG and non-walrus SG are:
// 1. Walrus SG connects to multiple databases. Some databases may require password, but some don't.
//    In particular, "scratch" does not require password. c.f. walrus_config.json.
//    For non-walrus SG, we use only one database, "scatch", and it requires password. For test cases
//    that use databases that require password in the walrus case, we run them in the unique database,
//    namely "scratch," with unqiue user with the same user/password as specified in walrus_config.json.
// 2. In walrus case, we flush the databses prefixed by "scratch_" because tests may push test documents
//    to them. Flushing a database is quite time-expensive in non-walrus case. Therefore, we don't
//    flush them, but, instead, we ensure that each test pushes documents with unique doc IDs to keep
//    the documents separate among tests
// To run the tests in "NOT_WALRUS" mode, the v3.1 SG must be configured without using the collections.

class ReplicatorWalrusTest : public ReplicatorAPITest {
public:
    ReplicatorWalrusTest() {
        if (getenv("USE_CLIENT_CERT")) {
#ifdef COUCHBASE_ENTERPRISE
            REQUIRE(Address::isSecure(_sg.address));
            Identity ca = CertHelper::readIdentity(sReplicatorFixturesDir + "ca_cert.pem",
                                                   sReplicatorFixturesDir + "ca_key.pem",
                                                   "Couchbase");
            // The Common Name in the client cert has to be the email address of a user account
            // in Sync Gateway, or you only get guest access.
            Identity id = CertHelper::createIdentity(false, kC4CertUsage_TLSClient,
                                                     "Pupshaw", "pupshaw@couchbase.org", &ca);
            _sg.identityCert = id.cert;
            _sg.identityKey  = id.key;
#else
            FAIL("USE_CLIENT_CERT only works with EE builds");
#endif
        }
    }

    enum AuthType {
        kAuthNone,
        kAuthBody,
        kAuthHeader
    };

    void notWalrus(AuthType authType =kAuthNone) {
        _flushedScratch = true;
        _sg.pinnedCert = C4Test::readFile(sReplicatorFixturesDir + "cert/cert.pem");
        if(getenv("NOTLS")) {
            _sg.address = {kC4Replicator2Scheme,
                           C4STR("localhost"),
                           4984};
        } else {
            _sg.address = {kC4Replicator2TLSScheme,
                           C4STR("localhost"),
                           4984};
        }
        switch (authType) {
            case kAuthBody: {
                Encoder enc;
                enc.beginDict();
                enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
                enc.beginDict();
                enc.writeKey(C4STR(kC4ReplicatorAuthType));
                enc.writeString("Basic"_sl);
                enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
                enc.writeString("sguser");
                enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
                enc.writeString("password");
                enc.endDict();
                enc.endDict();
                _options = AllocedDict(enc.finish());
            } break;

            case kAuthHeader: {
                Encoder enc;
                enc.beginDict();
                enc.writeKey(C4STR(kC4ReplicatorOptionExtraHeaders));
                enc.beginDict();
                enc.writeKey("Authorization"_sl);
                enc.writeString("Basic c2d1c2VyOnBhc3N3b3Jk"_sl);  // sguser:password
                enc.endDict();
                enc.endDict();
                _options = AllocedDict(enc.finish());
            } break;

            default:
                break;
        }
    }
};

TEST_CASE_METHOD(ReplicatorWalrusTest, "API Auth Failure", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus();
#else
    _sg.remoteDBName = kProtectedDBName;
#endif
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(_callbackStatus.error.domain == WebSocketDomain);
    CHECK(_callbackStatus.error.code == 401);
    CHECK(_headers["Www-Authenticate"].asString() == "Basic realm=\"Couchbase Sync Gateway\""_sl);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "API Auth Success", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus();
    SG::TestUser testUser {_sg, "pupshaw", {}, "frank"};
#else
    _sg.remoteDBName = kProtectedDBName;
#endif
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    replicate(kC4OneShot, kC4Disabled, true);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "API ExtraHeaders", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus();
#else
    _sg.remoteDBName = kProtectedDBName;
#endif
    // Use the extra-headers option to add HTTP Basic auth:
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionExtraHeaders));
    enc.beginDict();
    enc.writeKey("Authorization"_sl);
#ifdef NOT_WALRUS
    enc.writeString("Basic c2d1c2VyOnBhc3N3b3Jk"_sl);  // sguser:password
#else
    enc.writeString("Basic cHVwc2hhdzpmcmFuaw=="_sl);  // that's user 'pupshaw', password 'frank'
#endif
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    replicate(kC4OneShot, kC4Disabled, true);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "API Push Empty DB", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
#endif
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "API Push Non-Empty DB", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus(kAuthHeader);
#endif
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "API Push Empty Doc", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
#endif
    Encoder enc;
    enc.beginDict();
    enc.endDict();
    alloc_slice body = enc.finish();
    createRev("doc"_sl, kRevID, body);

    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "API Push Big DB", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", 0.0, false, nullptr, 0, idPrefix);
#else
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
#endif
    replicate(kC4OneShot, kC4Disabled);
}


#if 0
TEST_CASE_METHOD(ReplicatorWalrusTest, "API Push Large-Docs DB", "[.SyncServerWalrus]") {
    importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json");
    replicate(kC4OneShot, kC4Disabled);
}
#endif


TEST_CASE_METHOD(ReplicatorWalrusTest, "API Push 5000 Changes", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
#endif
    string docID = "Doc";
#ifdef NOT_WALRUS
    docID = idPrefix + docID;
#endif
    string revID;
    {
        TransactionHelper t(db);
        revID = createNewRev(db, slice(docID), nullslice, kFleeceBody);
    }
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Mutations --------");
    {
        TransactionHelper t(db);
        for (int i = 2; i <= 5000; ++i)
            revID = createNewRev(db, slice(docID), slice(revID), kFleeceBody);
    }

    C4Log("-------- Second Replication --------");
    replicate(kC4OneShot, kC4Disabled);
}

#ifndef NOT_WALRUS
// Involves pre-populated db.
TEST_CASE_METHOD(ReplicatorWalrusTest, "API Pull", "[.SyncServerWalrus]") {
    _sg.remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}
#endif

#ifndef NOT_WALRUS
// Involves pre-populated db
TEST_CASE_METHOD(ReplicatorWalrusTest, "API Pull With Indexes", "[.SyncServerWalrus]") {
    // Indexes slow down doc insertion, so they affect replicator performance.
    REQUIRE(c4db_createIndex(db, C4STR("Name"),   C4STR("[[\".Name\"]]"), kC4FullTextIndex, nullptr, nullptr));
    REQUIRE(c4db_createIndex(db, C4STR("Artist"), C4STR("[[\".Artist\"]]"), kC4ValueIndex, nullptr, nullptr));
    REQUIRE(c4db_createIndex(db, C4STR("Year"),   C4STR("[[\".Year\"]]"), kC4ValueIndex, nullptr, nullptr));

    _sg.remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}
#endif

TEST_CASE_METHOD(ReplicatorWalrusTest, "API Continuous Push", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    importJSONLines(sFixturesDir + "names_100.json", 0.0, false, nullptr, 0, idPrefix);
#else
    importJSONLines(sFixturesDir + "names_100.json");
#endif
    _stopWhenIdle = true;
    replicate(kC4Continuous, kC4Disabled);
}

#ifndef NOT_WALRUS
// Test requires pre-installed db.
TEST_CASE_METHOD(ReplicatorWalrusTest, "API Continuous Pull", "[.SyncServerWalrus]") {
    _sg.remoteDBName = kITunesDBName;
    _stopWhenIdle = true;
    replicate(kC4Disabled, kC4Continuous);
}
#endif

#ifndef NOT_WALRUS
TEST_CASE_METHOD(ReplicatorWalrusTest, "API Continuous Pull Forever", "[.SyncServer_Special]") {
    _sg.remoteDBName = kScratchDBName;
    _stopWhenIdle = false;  // This test will NOT STOP ON ITS OWN
    _mayGoOffline = true;
    replicate(kC4Disabled, kC4Continuous);
    // For CBL-2204: Wait for replicator to go idle, then shut down (Ctrl-C) SG process.
}
#endif


TEST_CASE_METHOD(ReplicatorWalrusTest, "Stop after Idle with Error", "[.SyncServerWalrus]") {
    // CBL-2501. This test is motivated by this bug. The bug bites when it finds a network error as the replicator
    // closes the socket after being stopped. Not able to find a way to inject the error, I tested
    // this case by tempering with the code in WebSocketImpl.onClose() and inject a transient error,
    // CloseStatus { kWebSocketClose, kCodeAbnormal }
    // Before the fix: continuous retry after Stopping;
    // after the fix: stop with the error regardless of it being transient.
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
#else
    _sg.remoteDBName = kScratchDBName;
#endif
    _mayGoOffline = true;
    _stopWhenIdle = true;
    ReplParams replParams { kC4Disabled, kC4Continuous };
#ifdef NOT_WALRUS
    std::unordered_map<alloc_slice, unsigned> docIDs {
        {alloc_slice(idPrefix), 1}
    };
    replParams.setDocIDs(docIDs);
#endif
    replicate(replParams, false);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Push & Pull Deletion", "[.SyncServerWalrus]") {
    string docID {"doc"};
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
#endif
    createRev(slice(docID), kRevID, kFleeceBody);
    createRev(slice(docID), kRev2ID, kEmptyFleeceBody, kRevDeleted);

    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();
    createRev(slice(docID), kRevID, kFleeceBody);
    ReplParams replParams { kC4Disabled, kC4OneShot };
#ifdef NOT_WALRUS
    auto docIDs = ReplicatorSGTest::getDocIDs(db);
    replParams.setDocIDs(docIDs);
#endif
    replicate(replParams);
    
    c4::ref<C4Document> doc = c4db_getDoc(db, slice(docID), true, kDocGetAll, nullptr);
    REQUIRE(doc);

    CHECK(doc->revID == kRev2ID);
    CHECK((doc->flags & kDocDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == kRevID);
}

TEST_CASE_METHOD(ReplicatorWalrusTest, "Push & Pull Attachments", "[.SyncServerWalrus]") {
    string docID {"att1"};
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
#endif
    std::vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    std::vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments(slice(docID), attachments, "text/plain");
    }

    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db, slice(docID), true, ERROR_INFO(error));
    REQUIRE(doc);
    alloc_slice before = c4doc_bodyAsJSON(doc, true, ERROR_INFO(error));
    CHECK(before);
    doc = nullptr;
    C4Log("Original doc: %.*s", SPLAT(before));

    replicate(kC4OneShot, kC4Disabled);
#ifdef NOT_WALRUS
    auto docIDs = ReplicatorSGTest::getDocIDs(db);
#endif
    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();

    ReplParams replParams { kC4Disabled, kC4OneShot };
#ifdef NOT_WALRUS
    replParams.setDocIDs(docIDs);
#endif
    replicate(replParams);

    doc = c4doc_get(db, slice(docID), true, ERROR_INFO(error));
    REQUIRE(doc);
    alloc_slice after = c4doc_bodyAsJSON(doc, true, ERROR_INFO(error));
    CHECK(after);
    C4Log("Pulled doc: %.*s", SPLAT(after));

    // Is the pulled identical to the original?
    CHECK(after == before);

    // Did we get all of its attachments?
    auto blobStore = c4db_getBlobStore(db, ERROR_INFO(error));
    REQUIRE(blobStore);
    for( auto key : blobKeys) {
        alloc_slice blob = c4blob_getContents(blobStore, key, ERROR_INFO(error));
        CHECK(blob);
    }
}

TEST_CASE_METHOD(ReplicatorWalrusTest, "Prove Attachments", "[.SyncServerWalrus]") {
    string doc1 = "doc one";
    string doc2 = "doc two";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    doc1 = idPrefix + doc1;
    doc2 = idPrefix + doc2;
#endif
    std::vector<string> attachments = {"Hey, this is an attachment!"};
    {
        TransactionHelper t(db);
        addDocWithAttachments(slice(doc1), attachments, "text/plain");
    }
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Creating 2nd doc with same attachments --------");

    {
        TransactionHelper t(db);
        addDocWithAttachments(slice(doc2), attachments, "text/plain");
    }
    // Pushing the second doc will cause Sync Gateway to ask for proof (send "proveAttachment")
    // instead of requesting the attachment itself, since it already has the attachment.
    replicate(kC4OneShot, kC4Disabled);
}

#ifndef NOT_WALRUS
// The test requires pre-populated db
TEST_CASE_METHOD(ReplicatorWalrusTest, "API Pull Big Attachments", "[.SyncServerWalrus]") {
    _sg.remoteDBName = kImagesDBName;
    replicate(kC4Disabled, kC4OneShot);

    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db, "Abstract"_sl, true, ERROR_INFO(error));
    REQUIRE(doc);
    Dict root = c4doc_getProperties(doc);
    auto attach = root.get("_attachments"_sl).asDict().get("Abstract.jpg"_sl).asDict();
    REQUIRE(attach);
    CHECK(attach.get("content_type").asString() == "image/jpeg"_sl);
    slice digest = attach.get("digest").asString();
    CHECK(digest == "sha1-9g3HeOewh8//ctPcZkh03o+A+PQ="_sl);
    C4BlobKey blobKey;
    c4blob_keyFromString(digest, &blobKey);
    auto size = c4blob_getSize(c4db_getBlobStore(db, nullptr), blobKey);
    CHECK(size == 15198281);

    C4Log("-------- Pushing --------");
    _sg.remoteDBName = kScratchDBName;
    replicate(kC4OneShot, kC4Disabled);
}
#endif

TEST_CASE_METHOD(ReplicatorWalrusTest, "API Push Conflict", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    importJSONLines(sFixturesDir + "names_100.json", 0.0, false, nullptr, 0, idPrefix);
#else
    importJSONLines(sFixturesDir + "names_100.json");
#endif
    const string originalRevID = "1-3cb9cfb09f3f0b5142e618553966ab73539b8888";

    string doc13 = "0000013";
#ifdef NOT_WALRUS
    doc13 = idPrefix + doc13;
    _sg.authHeader = HTTPLogic::basicAuth("sguser", "password");
#endif
    replicate(kC4OneShot, kC4Disabled);

    _sg.sendRemoteRequest("PUT", doc13, "{\"_rev\":\"" + originalRevID + "\","
                                          "\"serverSideUpdate\":true}", false, HTTPStatus::Created);

    createRev(slice(doc13), "2-f000"_sl, kFleeceBody);

    c4::ref<C4Document> doc = c4db_getDoc(db, slice(doc13), true, kDocGetAll, nullptr);
    REQUIRE(doc);
	C4Slice revID = C4STR("2-f000");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(c4doc_getProperties(doc) != nullptr);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = slice(originalRevID);
    CHECK(doc->selectedRev.revID == revID);
    CHECK(c4doc_getProperties(doc) != nullptr);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);

    C4Log("-------- Pushing Again (conflict) --------");
    _expectedDocPushErrors = {doc13};
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Pulling --------");
    _expectedDocPushErrors = { };
    _expectedDocPullErrors = {doc13};
    ReplParams replParams { kC4Disabled, kC4OneShot };
#ifdef NOT_WALRUS
    auto docIDs = ReplicatorSGTest::getDocIDs(db);
    replParams.setDocIDs(docIDs);
#endif
    replicate(replParams);

    C4Log("-------- Checking Conflict --------");
    doc = c4db_getDoc(db, slice(doc13), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK((doc->flags & kDocConflicted) != 0);
	revID = C4STR("2-f000");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(c4doc_getProperties(doc) != nullptr);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = slice(originalRevID);
    CHECK(doc->selectedRev.revID == revID);
#if 0 // FIX: These checks fail due to issue #402; re-enable when fixing that bug
    CHECK(c4doc_getProperties(doc) != nullptr);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
#endif
    REQUIRE(c4doc_selectCurrentRevision(doc));
    REQUIRE(c4doc_selectNextRevision(doc));
	revID = C4STR("2-883a2dacc15171a466f76b9d2c39669b");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
    CHECK(c4doc_getProperties(doc) != nullptr);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = slice(originalRevID);
    CHECK(doc->selectedRev.revID == revID);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Update Once-Conflicted Doc", "[.SyncServerWalrus]") {
    string docID = "doc";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
#endif
    const string path = docID + "?new_edits=false";
    // For issue #448.
    // Create a conflicted doc on SG, and resolve the conflict:
#ifdef NOT_WALRUS
    _sg.authHeader = HTTPLogic::basicAuth("sguser", "password");
#else
    _sg.remoteDBName = "scratch_allows_conflicts"_sl;
    flushScratchDatabase();
#endif
    _sg.sendRemoteRequest("PUT", path, "{\"_rev\":\"1-aaaa\",\"foo\":1}"_sl, false, HTTPStatus::Created);
    _sg.sendRemoteRequest("PUT", path, "{\"_revisions\":{\"start\":2,\"ids\":[\"bbbb\",\"aaaa\"]},\"foo\":2.1}"_sl, false, HTTPStatus::Created);
    _sg.sendRemoteRequest("PUT", path, "{\"_revisions\":{\"start\":2,\"ids\":[\"cccc\",\"aaaa\"]},\"foo\":2.2}"_sl, false, HTTPStatus::Created);
    _sg.sendRemoteRequest("PUT", path, "{\"_revisions\":{\"start\":3,\"ids\":[\"dddd\",\"cccc\"]},\"_deleted\":true}"_sl, false, HTTPStatus::Created);

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams { kC4OneShot, kC4OneShot };
#ifdef NOT_WALRUS
    std::unordered_map<alloc_slice, unsigned> docIDs {
        {alloc_slice(docID), 1}
    };
    replParams.setDocIDs(docIDs);
#endif
    replicate(replParams);

    // Verify doc:
    c4::ref<C4Document> doc = c4db_getDoc(db, slice(docID), true, kDocGetAll, nullptr);
    REQUIRE(doc);
	C4Slice revID = C4STR("2-bbbb");
    CHECK(doc->revID == revID);
    CHECK((doc->flags & kDocDeleted) == 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == "1-aaaa"_sl);

    // Update doc:
    createRev(slice(docID), "3-ffff"_sl, kFleeceBody);

    // Push change back to SG:
    C4Log("-------- Pushing");
    replicate(replParams);

    // Verify doc is updated on SG:
    auto body = _sg.sendRemoteRequest("GET", docID);
    string expectedBody = R"({"_id":")" + docID + R"(","_rev":"3-ffff","ans*wer":42})";
    CHECK(string(body) == expectedBody);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Pull multiply-updated", "[.SyncServerWalrus]") {
    // From <https://github.com/couchbase/couchbase-lite-core/issues/652>:
    // 1. Setup CB cluster & Configure SG
    // 2. Create a document using POST API via SG
    // 3. Create a cblite db on local server using cblite serve
    //      ./cblite/build/cblite serve  --create db.cblite2
    // 4. Replicate between SG -> db.cblite2
    //      ./cblite/build/cblite pull  ws://172.23.100.204:4985/db db.cblite2
    // 5. Validate number of records on db.cblite2 ->Should be  equal to number of documents created in Step2
    // 6. Update existing document using update API via SG (more than twice)
    //      PUT sghost:4985/bd/doc_id?=rev_id
    // 7. run replication between SG -> db.cblite2 again
    string docID = "doc";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
    _sg.authHeader = HTTPLogic::basicAuth("sguser", "password");
#else
    flushScratchDatabase();
#endif
    _sg.sendRemoteRequest("PUT", docID+"?new_edits=false", "{\"count\":1, \"_rev\":\"1-1111\"}"_sl, false, HTTPStatus::Created);

    ReplParams replParams { kC4Disabled, kC4OneShot };
#ifdef NOT_WALRUS
    std::unordered_map<alloc_slice, unsigned> docIDs { {alloc_slice(docID), 1} };
    replParams.setDocIDs(docIDs);
#endif
    replicate(replParams);
    c4::ref<C4Document> doc = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "1-1111"_sl);

    _sg.sendRemoteRequest("PUT", docID, "{\"count\":2, \"_rev\":\"1-1111\"}"_sl, false, HTTPStatus::Created);
    _sg.sendRemoteRequest("PUT", docID, "{\"count\":3, \"_rev\":\"2-c5557c751fcbfe4cd1f7221085d9ff70\"}"_sl, false, HTTPStatus::Created);
    _sg.sendRemoteRequest("PUT", docID, "{\"count\":4, \"_rev\":\"3-2284e35327a3628df1ca8161edc78999\"}"_sl, false, HTTPStatus::Created);

    replicate(replParams);
    doc = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "4-ffa3011c5ade4ec3a3ec5fe2296605ce"_sl);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Pull deltas from SG", "[.SyncServerWalrus][Delta]") {
    static constexpr int kNumDocs = 1000, kNumProps = 1000;
#ifdef NOT_WALRUS
    const string idPrefix = ReplicatorSGTest::timePrefix();
    notWalrus(kAuthBody);
    _sg.authHeader = HTTPLogic::basicAuth("sguser", "password");
#else
    flushScratchDatabase();
    _logRemoteRequests = false;
#endif

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        std::srand(123456); // start random() sequence at a known place
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            char docID[kDocBufSize];
#ifdef NOT_WALRUS
            snprintf(docID, kDocBufSize, "%sdoc-%03d", idPrefix.c_str(), docNo);
#else
            snprintf(docID, kDocBufSize, "doc-%03d", docNo);
#endif
            Encoder enc(c4db_createFleeceEncoder(db));
            enc.beginDict();
            for (int p = 0; p < kNumProps; ++p) {
                enc.writeKey(format("field%03d", p));
                enc.writeInt(std::rand());
            }
            enc.endDict();
            alloc_slice body = enc.finish();
            string revID = createNewRev(db, slice(docID), body);
        }
    };
    populateDB();

    C4Log("-------- Pushing to SG --------");
    ReplParams replParams { kC4OneShot, kC4Disabled };
#ifdef NOT_WALRUS
    auto docIDs = ReplicatorSGTest::getDocIDs(db);
    replParams.setDocIDs(docIDs);
#endif
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    {
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            char docID[kDocBufSize];
#ifdef NOT_WALRUS
            snprintf(docID, kDocBufSize, "%sdoc-%03d", idPrefix.c_str(), docNo);
#else
            snprintf(docID, kDocBufSize, "doc-%03d", docNo);
#endif
            C4Error error;
            c4::ref<C4Document> doc = c4doc_get(db, slice(docID), false, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for (Dict::iterator i(props); i; ++i) {
                enc.writeKey(i.keyString());
                auto value = i.value().asInt();
                if (RandomNumber() % 8 == 0)
                    value = RandomNumber();
                enc.writeInt(value);
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        REQUIRE(_sg.insertBulkDocs(enc.finish(), 30));
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            replParams.setOption(kC4ReplicatorOptionDisableDeltas, true);
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDB();
        populateDB();
        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;
        replParams.setPushPull(kC4Disabled, kC4OneShot);
        replicate(replParams);
        double time = st.elapsed();
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, kNumDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;

        int n = 0;
        C4Error error;
        c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(db, nullptr, ERROR_INFO(error));
        REQUIRE(e);
        while (c4enum_next(e, ERROR_INFO(error))) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
#ifdef NOT_WALRUS
            CHECK(slice(info.docID).hasPrefix(slice(idPrefix+"doc-")));
#else
            CHECK(slice(info.docID).hasPrefix("doc-"_sl));
#endif
            CHECK(slice(info.revID).hasPrefix("2-"_sl));
            ++n;
        }
        CHECK(error.code == 0);
        CHECK(n == kNumDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
          timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Pull iTunes deltas from SG", "[.SyncServerWalrus][Delta]") {
#ifdef NOT_WALRUS
    const string idPrefix = ReplicatorSGTest::timePrefix();
    notWalrus(kAuthBody);
    _sg.authHeader = HTTPLogic::basicAuth("sguser", "password");
#else
    flushScratchDatabase();
    _logRemoteRequests = false;
#endif

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
#ifdef NOT_WALRUS
        importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", 0.0, false, nullptr, 0, idPrefix);
#else
        importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
#endif
    };
    populateDB();
    auto numDocs = c4db_getDocumentCount(db);

    C4Log("-------- Pushing to SG --------");
    ReplParams replParams { kC4OneShot, kC4Disabled };
#ifdef NOT_WALRUS
    auto docIDs = ReplicatorSGTest::getDocIDs(db);
    replParams.setDocIDs(docIDs);
#endif
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    {
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for (int docNo = 0; docNo < numDocs; ++docNo) {
            char docID[kDocBufSize];
#ifdef NOT_WALRUS
            snprintf(docID, kDocBufSize, "%s%07u", idPrefix.c_str(), docNo + 1);
#else
            snprintf(docID, kDocBufSize, "%07u", docNo + 1);
#endif
            C4Error error;
            c4::ref<C4Document> doc = c4doc_get(db, slice(docID), false, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for (Dict::iterator i(props); i; ++i) {
                enc.writeKey(i.keyString());
                auto value = i.value();
                if (i.keyString() == "Play Count"_sl)
                    enc.writeInt(value.asInt() + 1);
                else
                    enc.writeValue(value);
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        REQUIRE(_sg.insertBulkDocs(enc.finish(), 120));
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            replParams.setOption(kC4ReplicatorOptionDisableDeltas, true);
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDB();
        populateDB();
        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;
        replParams.setPushPull(kC4Disabled, kC4OneShot);
        replicate(replParams);
        double time = st.elapsed();
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, numDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;

        int n = 0;
        C4Error error;
        c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(db, nullptr, ERROR_INFO(error));
        REQUIRE(e);
        while (c4enum_next(e, ERROR_INFO(error))) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            //CHECK(slice(info.docID).hasPrefix("doc-"_sl));
            CHECK(slice(info.revID).hasPrefix("2-"_sl));
            ++n;
        }
        CHECK(error.code == 0);
        CHECK(n == numDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
          timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}

// This test requires SG 3.0
TEST_CASE_METHOD(ReplicatorWalrusTest, "Auto Purge Enabled - Revoke Access", "[.SyncServerWalrus]") {
    string docID = "doc1";
    string channelIDa = "a";
    string channelIDb = "b";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
    channelIDa = idPrefix + channelIDa;
    channelIDb = idPrefix + channelIDb;
    SG::TestUser testUser {_sg, "pupshaw", {channelIDa, channelIDb}, "frank"};
#else
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
#endif

    // Create docs on SG:
    _sg.authHeader = "Basic cHVwc2hhdzpmcmFuaw=="_sl;
    REQUIRE(_sg.upsertDoc(docID, "{}", {channelIDa, channelIDb}));

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };
    
    // Setup pull filter:
    _pullFilter = [](C4String collectionName, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };
    
    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4Disabled, kC4OneShot);

    // Verify:
    c4::ref<C4Document> doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("1-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    
    // Revoked access to channel 'a':
    HTTPStatus status;
    C4Error error;
#ifdef NOT_WALRUS
    REQUIRE(testUser.setChannels({channelIDb}));
#else
    _sg.sendRemoteRequest("PUT", "_user/pupshaw", &status, &error, "{\"admin_channels\":[\"b\"]}"_sl, true);
    REQUIRE(status == HTTPStatus::OK);
#endif
    
    // Check if update to doc1 is still pullable:
    auto oRevID = slice(doc1->revID).asString();
    REQUIRE(_sg.upsertDoc(docID, oRevID, "{}", {channelIDb}));
    
    C4Log("-------- Pull update");
    replicate(kC4Disabled, kC4OneShot);
    
    // Verify the update:
    doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    
    // Revoke access to all channels:
    _sg.sendRemoteRequest("PUT", "_user/pupshaw", &status, &error, "{\"admin_channels\":[]}"_sl, true);
    REQUIRE(status == HTTPStatus::OK);

    C4Log("-------- Pull the revoked");
    replicate(kC4Disabled, kC4OneShot);
    
    // Verify if doc1 is purged:
    doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(!doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}

// This test requires SG 3.0
TEST_CASE_METHOD(ReplicatorWalrusTest, "Auto Purge Enabled - Filter Revoked Revision", "[.SyncServerWalrus]") {
    string docID = "doc1";
    string channelIDa = "a";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
    channelIDa = idPrefix + channelIDa;
    SG::TestUser testUser {_sg, "pupshaw", {channelIDa}, "frank"};
#else
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
#endif

    // Create docs on SG:
    _sg.authHeader = "Basic cHVwc2hhdzpmcmFuaw=="_sl;
    REQUIRE(_sg.upsertDoc(docID, "{}", {channelIDa}));

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());
    
    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };
    
    // Setup pull filter to filter the _removed rev:
    _pullFilter = [](C4String collectionName, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            return false;
        }
        return true;
    };
    
    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4Disabled, kC4OneShot);

    // Verify:
    c4::ref<C4Document> doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    
    // Revoke access to all channels:
    HTTPStatus status;
    C4Error error;
    _sg.sendRemoteRequest("PUT", "_user/pupshaw", &status, &error, "{\"admin_channels\":[]}"_sl, true);
    REQUIRE(status == HTTPStatus::OK);
    
    C4Log("-------- Pull the revoked");
    replicate(kC4Disabled, kC4OneShot);
    
    // Verify if doc1 is not purged as the revoked rev is filtered:
    doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}

// This test requires SG 3.0
TEST_CASE_METHOD(ReplicatorWalrusTest, "Auto Purge Disabled - Revoke Access", "[.SyncServerWalrus]") {
    string docID = "doc1";
    string channelIDa = "a";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
    channelIDa = idPrefix + channelIDa;
    SG::TestUser testUser {_sg, "pupshaw", {channelIDa}, "frank"};
#else
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
#endif

    // Create docs on SG:
    _sg.authHeader = "Basic cHVwc2hhdzpmcmFuaw=="_sl;
    REQUIRE(_sg.upsertDoc(docID, "{}", {channelIDa}));

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAutoPurge));
        enc.writeBool(false);
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());
    
    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };
    
    // Setup pull filter:
    _pullFilter = [](C4String collectionName, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
        }
        return true;
    };
    
    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4Disabled, kC4OneShot);

    // Verify:
    c4::ref<C4Document> doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    
    // Revoke access to all channels:
    HTTPStatus status;
    C4Error error;
    _sg.sendRemoteRequest("PUT", "_user/pupshaw", &status, &error, "{\"admin_channels\":[]}"_sl, true);
    REQUIRE(status == HTTPStatus::OK);
    
    C4Log("-------- Pulling the revoked");
    replicate(kC4Disabled, kC4OneShot);
    
    // Verify if the doc1 is not purged as the auto purge is disabled:
    doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    // No pull filter called
    CHECK(_counter == 0);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Auto Purge Enabled - Remove Doc From Channel", "[.SyncServerWalrus]") {
    string docID = "doc1";
    string channelIDa = "a";
    string channelIDb = "b";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
    channelIDa = idPrefix + channelIDa;
    channelIDb = idPrefix + channelIDb;
    SG::TestUser testUser {_sg, "pupshaw", {channelIDa, channelIDb}, "frank"};
#else
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
#endif

    // Create docs on SG:
    _sg.authHeader = "Basic cHVwc2hhdzpmcmFuaw=="_sl;
    REQUIRE(_sg.upsertDoc(docID, "{}", {channelIDa, channelIDb}));

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());
    
    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };
    
    // Setup pull filter:
    _pullFilter = [](C4String collectionName, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };
    
    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4Disabled, kC4OneShot);

    // Verify:
    c4::ref<C4Document> doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("1-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    
    // Removed doc from channel 'a':
    auto oRevID = slice(doc1->revID).asString();
    REQUIRE(_sg.upsertDoc(docID, oRevID, "{}", {channelIDb}));

    C4Log("-------- Pull update");
    replicate(kC4Disabled, kC4OneShot);
    
    // Verify the update:
    doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    
    // Remove doc from all channels:
    oRevID = slice(doc1->revID).asString();
    REQUIRE(_sg.upsertDoc(docID, oRevID, "{}", {}));
    
    C4Log("-------- Pull the removed");
    replicate(kC4Disabled, kC4OneShot);

    // Verify if doc1 is purged:
    doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(!doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Auto Purge Enabled - Filter Removed Revision", "[.SyncServerWalrus]") {
    string docID = "doc1";
    string channelIDa = "a";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
    channelIDa = idPrefix + channelIDa;
    SG::TestUser testUser {_sg, "pupshaw", {channelIDa}, "frank"};
#else
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
#endif

    // Create docs on SG:
    _sg.authHeader = "Basic cHVwc2hhdzpmcmFuaw=="_sl;
    REQUIRE(_sg.upsertDoc(docID, "{}", {channelIDa}));

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());
    
    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };
    
    // Setup pull filter to filter the _removed rev:
    _pullFilter = [](C4String collectionName, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
            return false;
        }
        return true;
    };
    
    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4Disabled, kC4OneShot);

    // Verify:
    c4::ref<C4Document> doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    
    // Remove doc from all channels
    auto oRevID = slice(doc1->revID).asString();
    _sg.sendRemoteRequest("PUT", docID, "{\"_rev\":\"" + oRevID + "\", \"channels\":[]}", false, HTTPStatus::Created);
    
    C4Log("-------- Pull the removed");
    replicate(kC4Disabled, kC4OneShot);
    
    // Verify if doc1 is not purged as the removed rev is filtered:
    doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Auto Purge Disabled - Remove Doc From Channel", "[.SyncServerWalrus]") {
    string docID = "doc1";
    string channelIDa = "a";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docID = idPrefix + docID;
    channelIDa = idPrefix + channelIDa;
    SG::TestUser testUser {_sg, "pupshaw", {channelIDa}, "frank"};
#else
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
#endif

    // Create docs on SG:
    _sg.authHeader = "Basic cHVwc2hhdzpmcmFuaw=="_sl;
    REQUIRE(_sg.upsertDoc(docID, "{}", {channelIDa}));

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAutoPurge));
        enc.writeBool(false);
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());
    
    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };
    
    // Setup pull filter:
    _pullFilter = [](C4String collectionName, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
        }
        return true;
    };
    
    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4Disabled, kC4OneShot);

    // Verify:
    c4::ref<C4Document> doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    
    // Remove doc from all channels
    auto oRevID = slice(doc1->revID).asString();
    _sg.sendRemoteRequest("PUT", docID, "{\"_rev\":\"" + oRevID + "\", \"channels\":[]}", false, HTTPStatus::Created);
    
    C4Log("-------- Pulling the removed");
    replicate(kC4Disabled, kC4OneShot);
    
    // Verify if the doc1 is not purged as the auto purge is disabled:
    doc1 = c4doc_get(db, slice(docID), true, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    // No pull filter called
    CHECK(_counter == 0);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Auto Purge Enabled(default) - Delete Doc", "[.SyncServerWalrus]") {
    string docIDStr = "doc";
    string channelIDa = "a";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docIDStr = idPrefix + docIDStr;
    channelIDa = idPrefix + channelIDa;
    SG::TestUser testUser {_sg, "pupshaw", {channelIDa}, "frank"};
#else
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
#endif

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Create a doc and push it:
    c4::ref<C4Document> doc;
    FLSlice docID = slice(docIDStr);
    string channelJSON = R"({channels:[')"+channelIDa+R"(']})";
    {
        TransactionHelper t(db);
        C4Error error;
        doc = c4doc_create(db, docID, json2fleece(channelJSON.c_str()), 0, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
    }
    CHECK(c4db_getDocumentCount(db) == 1);
    replicate(kC4OneShot, kC4Disabled);

    // Delete the doc and push it:
    {
        TransactionHelper t(db);
        C4Error error;
        doc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
        REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    }
    CHECK(c4db_getDocumentCount(db) == 0);
    replicate(kC4OneShot, kC4Disabled);

    // Apply a pull and verify that the document is not purged.
    replicate(kC4Disabled, kC4OneShot);
    C4Error error;
    doc = c4db_getDoc(db, docID, true, kDocGetAll, ERROR_INFO(error));
    CHECK(error.code == 0);
    CHECK(doc != nullptr);
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    CHECK(c4db_getDocumentCount(db) == 0);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Auto Purge Enabled(default) - Delete then Create Doc", "[.SyncServerWalrus]") {
    string docIDStr = "doc";
    string channelIDa = "a";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    docIDStr = idPrefix + docIDStr;
    channelIDa = idPrefix + channelIDa;
    SG::TestUser testUser {_sg, "pupshaw", {channelIDa}, "frank"};
#else
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
#endif

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorAuthType));
            enc.writeString("Basic"_sl);
            enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
            enc.writeString("pupshaw");
            enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
            enc.writeString("frank");
        enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Create a new doc and push it:
    c4::ref<C4Document> doc;
    string channelJSON = R"({channels:[')"+channelIDa+R"(']})";
    FLSlice docID = slice(docIDStr);
    {
        TransactionHelper t(db);
        C4Error error;
        doc = c4doc_create(db, docID, json2fleece(channelJSON.c_str()), 0, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
    }
    CHECK(c4db_getDocumentCount(db) == 1);
    replicate(kC4OneShot, kC4Disabled);

    // Delete the doc and push it:
    {
        TransactionHelper t(db);
        C4Error error;
        doc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
        REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    }
    CHECK(c4db_getDocumentCount(db) == 0);
    replicate(kC4OneShot, kC4Disabled);

    // Create a new doc with the same id that was deleted:
    {
        TransactionHelper t(db);
        C4Error error;
        doc = c4doc_create(db, docID, json2fleece(channelJSON.c_str()), 0, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
    }
    CHECK(c4db_getDocumentCount(db) == 1);

    // Apply a pull and verify the document is not purged:
    replicate(kC4Disabled, kC4OneShot);
    C4Error error;
    c4::ref<C4Document> doc2 = c4db_getDoc(db, docID, true, kDocGetAll, ERROR_INFO(error));
    CHECK(error.code == 0);
    CHECK(doc2 != nullptr);
    CHECK(c4db_getDocumentCount(db) == 1);
    CHECK(doc2->revID == doc->revID);
}

TEST_CASE_METHOD(ReplicatorWalrusTest, "Pinned Certificate Failure", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    _sg.address = {kC4Replicator2TLSScheme,
                   C4STR("localhost"),
                   4984};
    notWalrus(kAuthBody);
#else
    if (!Address::isSecure(_sg.address)) {
        return;
    }
    flushScratchDatabase();
#endif

    // Using an unmatched pinned cert:
    _sg.pinnedCert =                                                               \
        "-----BEGIN CERTIFICATE-----\r\n"                                      \
        "MIICpDCCAYwCCQCskbhc/nbA5jANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\r\n" \
        "b2NhbGhvc3QwHhcNMjIwNDA4MDEwNDE1WhcNMzIwNDA1MDEwNDE1WjAUMRIwEAYD\r\n" \
        "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDQ\r\n" \
        "vl0M5D7ZglW76p428x7iQoSkhNyRBEjZgSqvQW3jAIsIElWu7mVIIAm1tpZ5i5+Q\r\n" \
        "CHnFLha1TDACb0MUa1knnGj/8EsdOADvBfdBq7AotypiqBayRUNdZmLoQEhDDsen\r\n" \
        "pEHMDmBrDsWrgNG82OMFHmjK+x0RioYTOlvBbqMAX8Nqp6Yu/9N2vW7YBZ5ovsr7\r\n" \
        "vdFJkSgUYXID9zw/MN4asBQPqMT6jMwlxR1bPqjsNgXrMOaFHT/2xXdfCvq2TBXu\r\n" \
        "H7evR6F7ayNcMReeMPuLOSWxA6Fefp8L4yDMW23jizNIGN122BgJXTyLXFtvg7CQ\r\n" \
        "tMnE7k07LLYg3LcIeamrAgMBAAEwDQYJKoZIhvcNAQELBQADggEBABdQVNSIWcDS\r\n" \
        "sDPXk9ZMY3stY9wj7VZF7IO1V57n+JYV1tJsyU7HZPgSle5oGTSkB2Dj1oBuPqnd\r\n" \
        "8XTS/b956hdrqmzxNii8sGcHvWWaZhHrh7Wqa5EceJrnyVM/Q4uoSbOJhLntLE+a\r\n" \
        "FeFLQkPpJxdtjEUHSAB9K9zCO92UC/+mBUelHgztsTl+PvnRRGC+YdLy521ST8BI\r\n" \
        "luKJ3JANncQ4pCTrobH/EuC46ola0fxF8G5LuP+kEpLAh2y2nuB+FWoUatN5FQxa\r\n" \
        "+4F330aYRvDKDf8r+ve3DtchkUpV9Xa1kcDFyTcYGKBrINtjRmCIblA1fezw59ZT\r\n" \
        "S5TnM2/TjtQ=\r\n"                                                     \
        "-----END CERTIFICATE-----\r\n";

    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUntrusted);
}


TEST_CASE_METHOD(ReplicatorWalrusTest, "Pinned Certificate Success", "[.SyncServerWalrus]") {
#ifdef NOT_WALRUS
    _sg.address = {kC4Replicator2TLSScheme,
                   C4STR("localhost"),
                   4984};
    notWalrus(kAuthBody);
#else
    if (!Address::isSecure(_sg.address)) {
        return;
    }
    flushScratchDatabase();
#endif

    // Leaf:
#ifdef NOT_WALRUS
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIICqzCCAZMCFCbvSAAFwn8RVp3Rn26N2VKOc1oGMA0GCSqGSIb3DQEBCwUAMBAx
DjAMBgNVBAMMBUludGVyMB4XDTIzMDEyNTE3MjUzNVoXDTMzMDEyMjE3MjUzNVow
FDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB
CgKCAQEAt8zuD5uA4gIGVronjX3krmyH34KqD+Gsj6vu5KvFS5+/yJ5DdLZGS7BX
MsGUCfHa6WFalLEfH7BTdaualJyQxGM1qYFOtW5L/5H7x/uJcAtVnrujc/kUAUKW
eI037q+WQmBPvnUxYix5o1qOxjs2F92Loq6UrWZxub/rxkPkLZOAkSfCos00eodO
+Hrbb8HtkW8sJg0nYMYqYiJnBFnN8EMXSLkUQ+8ph4LgYl+8vUX3hdbIRGUUKFjJ
8bAOruThPaUP32JB13b4ww4rZ7rNIqDzJ2TMi+YgetxTdichbwVChcHCGeXIq8DQ
v6Qt8lhD8g74zeMjGlUvrJb5cEhtEQIDAQABMA0GCSqGSIb3DQEBCwUAA4IBAQAK
dPpw5OP8sGocCs/P43o8rSkFJPn7LdTkfCTyBWyjp9WjWztBelPsTw99Stsy/bgr
LOFkNtimtZVlv0SWKO9ZXVjkVF3JdMsy2mRlTy9530Bk9H/UJChJaX2Q9cwNivZX
SJT7Psv+gypR1pwU6Mp0mELXunnQndsuaZ+mzHbzVcci+c3nO/7g4xRNWNbTeCas
gNI1Nqt21+/kWwgpkuBbphSJUrTKE1NkVMsh/bfzDNTe2UiDszuU1Aq1HuctHilJ
I2RIXDu4xLSHFyHtsn2OKQyLzCAUCTOlFzpwUgjj917chG4cLGiy0ARQh+6q1+lM
4oW1jtacEQ0hW1u2y2De
-----END CERTIFICATE-----)");
#else
    _sg.pinnedCert =                                                               \
        "-----BEGIN CERTIFICATE-----\r\n"                                      \
        "MIICoDCCAYgCCQDOqeOThcl0DTANBgkqhkiG9w0BAQsFADAQMQ4wDAYDVQQDDAVJ\r\n" \
        "bnRlcjAeFw0yMjA0MDgwNDE2MjNaFw0zMjA0MDUwNDE2MjNaMBQxEjAQBgNVBAMM\r\n" \
        "CWxvY2FsaG9zdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMt7VQ0j\r\n" \
        "74/GJVnTfC0YQZHeCFoZbZyJ/4KPOpe1UoqRQ1xNtllPMHf4ukIeNd3tS4CHQDqK\r\n" \
        "83a7uGXEOzY3JFaVRnTpMcHRMnpmZQLWZs+WMCP5fzI4EcaJjFmqQSUjfZiocdh/\r\n" \
        "n5vKc64bhKyUStE2CSObMnJ/L5mPY1JUAgxQrXtK4lw1T/ppV2m4hiutr+gkhXjc\r\n" \
        "Sam4DheuMg7hSUZSwh7VI253ev1Hp4JdSmndQHvle99S+N5jJ11NZnEuQxcImmOI\r\n" \
        "MBVfRFpREFPOH+JrqsnYSic2GQvv31nAJsXzYX2t/VT0a3TUes3B9OZfAVA7nMFA\r\n" \
        "r3E9mjVGYVtn7skCAwEAATANBgkqhkiG9w0BAQsFAAOCAQEADbjYO9VxOGZT5LAv\r\n" \
        "ON+U+2FPG5Tons1ubWslThROqml7CCfNKPVhZCwe0BUQLWc35NYvqVjoSAenCHu6\r\n" \
        "EUANfqtuNxQAoeDCaP1epGYZ8fakJXvuyTjek3RV2PeiuFUIZQP/HWGfI640kh4V\r\n" \
        "xvUBa3joelnt+KjDB/yJemmf0dIXJ0dLtFBTN+YVp4aSFTtzcbqh50H6BSAgSiWR\r\n" \
        "ocTu5YpDXHZ6ufaMTRa2HUcSmFeWi75sS6ySgECTbeld1/mFZcSf1zXHU9WFg39D\r\n" \
        "knQNR2i1cJMbMZ3GCRyB6y3SxFb7/9BS70DV3p4n5BjYMlhNnHJx4u1JUTLWgybV\r\n" \
        "qrV+HA==\r\n"                                                         \
        "-----END CERTIFICATE-----\r\n";
#endif
    replicate(kC4OneShot, kC4Disabled, true);
    
    // Intermediate:
#ifdef NOT_WALRUS
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDRzCCAi+gAwIBAgIUNts/9gIBEy+cXri5JRHZuXbRkPQwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjMwMTI1MTcyNTM1
WhcNMzMwMTIyMTcyNTM1WjAQMQ4wDAYDVQQDDAVJbnRlcjCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBAKfT6m0Nby0BMDU/IW4aGqAO5w2i+W5Vn6V2E4Og
lNqweBDg+pPWwGyacaGXgsWMcFtxtxsmBDVRIuLzgo/tXDtN7yNdlGVq9WiOtbWB
ovKq0KiFrOGXbKHLPyRahGulXwZ5eI4nLIwPoxk6+q8jEiRzcvAWbKz+Qy51Iygq
k8MRQ8OZkinmWKcJ31cBjMuPzNgPCWn18iU7jkes5M0rBTK4M98gkR2SaqAo1L1b
QDLiEZRWD0dlwxkLgIWqjFj1yW3iVf/jILPuS4XK4C6byGewSVsS5f7OjXDrAuVI
igEbhRlTNEmsTfYjGBLNkbPRNM0VWEMc9gmtzbT5VZr7Ir8CAwEAAaOBjDCBiTAP
BgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBRloKIjYpry1TzFRKj3gMhTfN2fjzBX
BgNVHSMEUDBOgBQWNMmtETrZ1TO4Q6L+7enjksvyGKEgpB4wHDEaMBgGA1UEAwwR
Q291Y2hiYXNlIFJvb3QgQ0GCFEdmMdLR5K2lSu89v4YGnYd/hWQTMA0GCSqGSIb3
DQEBCwUAA4IBAQCORuTuWd2nWEl1DjcpUVXnbE4S6xG4YjC5VfGj36Gj5bjjZj+y
S4TWigwLvc8Rokx+ZqLHyTgrPcLKl/6DrFNNGZC6ByMEDH0XQQWYCLHDAfgkhBng
qD8eZmZ8tYvkZHf4At35RGfiZAtJBNrfxFtKodT0SeUT+qwGcuVLU5B6vgsH/Gib
82cxMLnXcqbyX2rW2yGpypB8Qb+K8qaotFqxxRFRT0+n40Bh86G8ik5/vEuYvlnv
nLMtWOJixTekuOrOh8TB0DgDVIx9gGu4xv4SYGKqseb9z4teJpSaI7LKws0buuHu
G6SJD+EJQ4UPaeYNjnFeh0DNlIHBkkZhdDtw
-----END CERTIFICATE-----)");
#else
    _sg.pinnedCert =                                                               \
        "-----BEGIN CERTIFICATE-----\r\n"                                      \
        "MIIDFTCCAf2gAwIBAgIJANZ8gSANI5jNMA0GCSqGSIb3DQEBCwUAMA8xDTALBgNV\r\n" \
        "BAMMBFJvb3QwHhcNMjIwNDA4MDQxNjIzWhcNMzIwNDA1MDQxNjIzWjAQMQ4wDAYD\r\n" \
        "VQQDDAVJbnRlcjCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAOm1MUNQ\r\n" \
        "xZKOCXw93eB/pmyCk5kEV3+H8RQC5Nq7orHvnHL6D/YVfVsobZyHkMSP3FVzl0bo\r\n" \
        "s1s+8kCjJ7O+M3TpzuSL8y4uLSEPmZF5qY2N7QobabrKVYueFxFmOD7+ypILx2QC\r\n" \
        "+hWd3J3XiLiiXqOO2jtjtwwy2+pD21DjmcPHGC4GKyv8/jp7hH4MFF6ux1wRQej1\r\n" \
        "on5jJQNFERUFdfX3wAmZgjww8bfyCEkHxnyIfJjEhyOtMLGGNUu8Hms7az+uYT6I\r\n" \
        "S4Q6VeBJ5WTKyhk7aJB1Rl6zZbROvTIq+ZaxAJNwsIzd/HiaoTwFUe3EFilIeGFK\r\n" \
        "w3vnPwiq99tDBHsCAwEAAaNzMHEwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQU\r\n" \
        "WXW5x/ufCrRKhv3F5wBqY0JVUEswPwYDVR0jBDgwNoAUefIiQi9GC9aBspej7UJT\r\n" \
        "zQzs/mKhE6QRMA8xDTALBgNVBAMMBFJvb3SCCQD1tOzs5zPQ/zANBgkqhkiG9w0B\r\n" \
        "AQsFAAOCAQEAEJhO1fA0d8Hu/5IHTlsGfmtcXOyXDcQQVz/3FKWrTPgDOYeMMNbG\r\n" \
        "WqvuG4YxmXt/+2OC1IYK/slrIK5XXldfRu90UM4wVXeD3ATLS3AG0Z/+yPRGbUbF\r\n" \
        "y5+11nXySGyKdV1ik0KgLGeYf0cuJ/vu+/7mkj4mGDfmTQv+8/HYKNaOqgKuVRlf\r\n" \
        "LHBh/RlbHMBn2nwL79vbrIeDaQ0zq9srt9F3CEy+SvlxX63Txmrym3fqTQjPUi5s\r\n" \
        "rEsy+eNr4N+aDWqGRcUkbP/C/ktGGNBHYG1NaPJq7CV1tdLe+usIcRWRR9vOBWbr\r\n" \
        "EkBGJMvCdhlWRv2FnrQ+VUQ+mhYHBS2Kng==\r\n"                             \
        "-----END CERTIFICATE-----\r\n";
#endif
    replicate(kC4OneShot, kC4Disabled, true);

    // Root:
#ifdef NOT_WALRUS
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDUzCCAjugAwIBAgIUR2Yx0tHkraVK7z2/hgadh3+FZBMwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjMwMTI1MTcyNTM1
WhcNMzMwMTIyMTcyNTM1WjAcMRowGAYDVQQDDBFDb3VjaGJhc2UgUm9vdCBDQTCC
ASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBANnHe9guNaE6Epcchx72GJy3
Tn4lmd0tcCBviZIti4FfyFu2tFai6S7Mj0JHWltuaLv5AD402dxb8gxG3ZKIPOPt
b38I/yJbQSs+ND3Ee056R5qnV22Fuw37X5Bu9+dZn1YgSM7lt1RnqpgW/yxLii8q
J5pRG6AUsIsr3NAE3EcLWcRA3kW1vinmm9bI1wD+lJBo9v3QJOXw+ndEWtcu5hqC
r4gQcGDvnOGTbaHOrhMIDgkl46gJSi3j2NNX093SlK23/84ZZmJOESHpE+1+JkeL
z6gawOmR8wHBlixOV1Y7SZrGPJ9Vp1cFqeUnDqButad+2C1cXZ2XlTUi5t32IIsC
AwEAAaOBjDCBiTAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBQWNMmtETrZ1TO4
Q6L+7enjksvyGDBXBgNVHSMEUDBOgBQWNMmtETrZ1TO4Q6L+7enjksvyGKEgpB4w
HDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0GCFEdmMdLR5K2lSu89v4YGnYd/
hWQTMA0GCSqGSIb3DQEBCwUAA4IBAQBIXmvcoWW0VZmjSEUmwFcyWq+38/AbPfRs
0MbhpHBvCau7/wOyTI/cq838yJYL+71BmXJNKFp8nF7Yc+PU6UkypXCsj2rHpblz
2bkjHJoEGw/HIPFo/ZywUiGfb/Jc6/t2PdHHBSkZO28oRnAt+q2Ehvqf/iT9bHO8
068JQXO5ttsA8JFQu26Thk/37559sruAn8/Lz3b8P6s6Ql3gg2LmCAh9v7gIcj64
kr6iDunu9X9glrd+1DV9otDwXh1iM2kd7MrCituUgTt7tclDFQMxuSSW2mc3k51Y
E1/H1T7j/M/LhIzUPNO80oPxLXl3TQFc+ZYwh5nSHeHbo91dY+vj
-----END CERTIFICATE-----)");
#else
    _sg.pinnedCert =                                                               \
        "-----BEGIN CERTIFICATE-----\r\n"                                      \
        "MIIDFDCCAfygAwIBAgIJAPW07OznM9D/MA0GCSqGSIb3DQEBCwUAMA8xDTALBgNV\r\n" \
        "BAMMBFJvb3QwHhcNMjIwNDA4MDQxNjIzWhcNMzIwNDA1MDQxNjIzWjAPMQ0wCwYD\r\n" \
        "VQQDDARSb290MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAvJV+Ptou\r\n" \
        "R1BS/0XXN+JImdNesaBJ2tcHrFHq2yK9V4qu2iUX8LgOcBpPg8yR0zJlzjwF+SLE\r\n" \
        "R8jBhD79YF8kF+r7cqBhsvy+e/ri0AaBiGsdP7NFPFEUCOukhnMIvLt10BvsRoCd\r\n" \
        "+eFrDZO0ZJer3ylp2GeB01rTgngWfrenhZdyGR8ISn+ijtN+J2IhAxsoLGDWiAL/\r\n" \
        "XWX55agSuAGi6zlomkReTMuyfkidLfrejUQCnrcDQQ7xqjdCB1QYBt6o1U1oHN3F\r\n" \
        "D6ICXirXJyVDJ2Ry6q+FrGJbJDUPlNwlPqAyukFFbeOINPKWiFQUw8nSo3i3DFMG\r\n" \
        "UZ3HhkQ/xfboZQIDAQABo3MwcTAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5\r\n" \
        "8iJCL0YL1oGyl6PtQlPNDOz+YjA/BgNVHSMEODA2gBR58iJCL0YL1oGyl6PtQlPN\r\n" \
        "DOz+YqETpBEwDzENMAsGA1UEAwwEUm9vdIIJAPW07OznM9D/MA0GCSqGSIb3DQEB\r\n" \
        "CwUAA4IBAQANxGwoeEBaibMQAqSWPnDBISiwk9uKy3buateXOtLlBSpM9ohE4iPG\r\n" \
        "GDFZ+9LoKJGy4vWmv6XD4zBeoqZ9hOgnvdEu0P+JITffjXCsfb0JPsOOjwbcJ+5+\r\n" \
        "TnfoXCyPRTEi/6OG1sKO2ibav5vMTUuUDdVYbPA2hfEAdn/n0GrN4fQ1USMKk+Ld\r\n" \
        "KWgWGZto+l0fKIXdHHpxr01V9Q/+6kzbpZOSxw41m/o1TwJxYSuRXZfK67YpBYGO\r\n" \
        "N4X2c7Qsvjd52vcZdRra+bkS0BJXwEDZZdmrZOlRAYIhE7lZ5ojqcZ+/UJztyPZq\r\n" \
        "Dbr9kMLDVeMuJfGyebdZ0zeMhVSv0PlD\r\n"                                 \
        "-----END CERTIFICATE-----\r\n";
#endif
    replicate(kC4OneShot, kC4Disabled, true);
}

#ifdef COUCHBASE_ENTERPRISE

static alloc_slice UnbreakableEncryption(slice cleartext, int8_t delta) {
    alloc_slice ciphertext(cleartext);
    for (size_t i = 0; i < ciphertext.size; ++i)
        (uint8_t&)ciphertext[i] += delta;        // "I've got patent pending on that!" --Wallace
    return ciphertext;
}

struct TestEncryptorContext {
    slice docID;
    slice keyPath;
    int called {0};
    std::optional<C4Error> simulateError;
};

static C4SliceResult testEncryptor(void* rawCtx,
                                   C4String documentID,
                                   FLDict properties,
                                   C4String keyPath,
                                   C4Slice input,
                                   C4StringResult* outAlgorithm,
                                   C4StringResult* outKeyID,
                                   C4Error* outError)
{
    auto context = (TestEncryptorContext*)((ReplicatorAPITest*)rawCtx)->_encCBContext;
    context->called++;
    CHECK(documentID == context->docID);
    CHECK(keyPath == context->keyPath);
    return C4SliceResult(UnbreakableEncryption(input, 1));
}

static C4SliceResult testEncryptorError(void* rawCtx,
                                        C4String documentID,
                                        FLDict properties,
                                        C4String keyPath,
                                        C4Slice input,
                                        C4StringResult* outAlgorithm,
                                        C4StringResult* outKeyID,
                                        C4Error* outError)
{
    auto context = (TestEncryptorContext*)((ReplicatorAPITest*)rawCtx)->_encCBContext;
    if (context->called++ == 0) {
        *outError = *context->simulateError;
        return C4SliceResult(nullslice);
    } else {
        CHECK(documentID == context->docID);
        CHECK(keyPath == context->keyPath);
        return C4SliceResult(UnbreakableEncryption(input, 1));
    }
}

static C4SliceResult testDecryptor(void* rawCtx,
                                   C4String documentID,
                                   FLDict properties,
                                   C4String keyPath,
                                   C4Slice input,
                                   C4String algorithm,
                                   C4String keyID,
                                   C4Error* outError)
{
    auto context = (TestEncryptorContext*)rawCtx;
    context->called++;
    CHECK(documentID == context->docID);
    CHECK(keyPath == context->keyPath);
    return C4SliceResult(UnbreakableEncryption(input, -1));
}

static C4SliceResult testDecryptorError(void* rawCtx,
                                        C4String documentID,
                                        FLDict properties,
                                        C4String keyPath,
                                        C4Slice input,
                                        C4String algorithm,
                                        C4String keyID,
                                        C4Error* outError)
{
    auto context = (TestEncryptorContext*)((ReplicatorAPITest*)rawCtx)->_decCBContext;
    if (context->called++ == 0) {
        *outError = *context->simulateError;
        return C4SliceResult(nullslice);
    } else {
        CHECK(documentID == context->docID);
        CHECK(keyPath == context->keyPath);
        return C4SliceResult(UnbreakableEncryption(input, -1));
    }
}

TEST_CASE_METHOD(ReplicatorWalrusTest, "Replicate Encryptor Error", "[.SyncServerWalrus]") {
    string doc1 = "doc01";
    string doc2 = "seekrit";
    string doc3 = "doc03";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    doc1 = idPrefix + doc1;
    doc2 = idPrefix + doc2;
    doc3 = idPrefix + doc3;
#endif

    slice originalJSON = R"({"SSN":{"@type":"encryptable","value":"123-45-6789"}})"_sl;
    slice unencryptedJSON = R"({"ans*wer": 42})"_sl;
    {
        TransactionHelper t(db);
        createFleeceRev(db, slice(doc1), kRevID, unencryptedJSON);
        createFleeceRev(db, slice(doc2), kRevID, originalJSON);
        createFleeceRev(db, slice(doc3), kRevID, unencryptedJSON);
    }

    TestEncryptorContext encryptContext = {doc2, "SSN"};
    _initParams.propertyEncryptor = &testEncryptorError;
    _encCBContext = &encryptContext;

    SECTION("LiteCoreDomain, kC4ErrorCrypto") {
        ExpectingExceptions x;
        encryptContext.simulateError = C4Error {LiteCoreDomain, kC4ErrorCrypto};
        _expectedDocPushErrors = { doc2 };
        replicate(kC4OneShot, kC4Disabled);
        CHECK(_callbackStatus.progress.documentCount == 2);
        CHECK(encryptContext.called == 1);

        // Try it again with good encryptor, but crypto errors will move the checkpoint
        // past the doc. The second attempt won't help.
        _initParams.propertyEncryptor = &testEncryptor;
        _expectedDocPushErrors = {};
        replicate(kC4OneShot, kC4Disabled);
        CHECK(_callbackStatus.progress.documentCount == 0);
        CHECK(encryptContext.called == 1);
    }

    SECTION("WebSocketDomain/503") {
        ExpectingExceptions x;
        encryptContext.simulateError = C4Error {WebSocketDomain, 503};
        _mayGoOffline = true;
        _expectedDocPushErrorsAfterOffline = { doc2 };
        replicate(kC4OneShot, kC4Disabled);
        CHECK(_wentOffline);
        CHECK(encryptContext.called == 2);

        Encoder enc;
        enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionDisablePropertyDecryption));
        enc.writeBool(true);
        // Copy any preexisting options:
        for (Dict::iterator i(_options); i; ++i) {
            enc.writeKey(i.keyString());
            enc.writeValue(i.value());
        }
        enc.endDict();
        _options = AllocedDict(enc.finish());
        ReplParams replParams { kC4Disabled, kC4OneShot };
#ifdef NOT_WALRUS
        auto docIDs = ReplicatorSGTest::getDocIDs(db);
        replParams.setDocIDs(docIDs);
#endif
        deleteAndRecreateDB();
        replicate(replParams);
        CHECK(c4db_getDocumentCount(db) == 3);

        // verify the content
        c4::ref<C4Document> doc = c4db_getDoc(db, slice(doc2), true, kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        Dict props = c4doc_getProperties(doc);
        CHECK(props.toJSON(false, true)
              == R"({"encrypted$SSN":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"IzIzNC41Ni43ODk6Iw=="}})"_sl);
        // Decrypt the "ciphertext" property by hand. We disabled decryption on the destination,
        // so the property won't be converted back from the server schema.
        slice       cipherb64 = props["encrypted$SSN"].asDict()["ciphertext"].asString();
        auto        cipher    = base64::decode(cipherb64);
        alloc_slice clear     = UnbreakableEncryption(cipher, -1);
        CHECK(clear == "\"123-45-6789\"");
    }
}

TEST_CASE_METHOD(ReplicatorWalrusTest, "Replicate Decryptor Error", "[.SyncServerWalrus]") {
    string doc1 = "doc01";
    string doc2 = "seekrit";
    string doc3 = "doc03";
#ifdef NOT_WALRUS
    notWalrus(kAuthBody);
    const string idPrefix = ReplicatorSGTest::timePrefix();
    doc1 = idPrefix + doc1;
    doc2 = idPrefix + doc2;
    doc3 = idPrefix + doc3;
#endif

    slice originalJSON = R"({"SSN":{"@type":"encryptable","value":"123-45-6789"}})"_sl;
    slice unencryptedJSON = R"({"ans*wer": 42})"_sl;
    {
        TransactionHelper t(db);
        createFleeceRev(db, slice(doc1), kRevID, unencryptedJSON);
        createFleeceRev(db, slice(doc2), kRevID, originalJSON);
        createFleeceRev(db, slice(doc3), kRevID, unencryptedJSON);
    }

    TestEncryptorContext encryptContext = {doc2, "SSN"};
    _initParams.propertyEncryptor = &testEncryptor;
    _encCBContext = &encryptContext;
    replicate(kC4OneShot, kC4Disabled);

    // check the 3 documents are pushed and clear the local db
    // Get ready for Pull/Decyption
    CHECK(c4db_getDocumentCount(db) == 3);
    ReplParams replParams { kC4Disabled, kC4OneShot };
#ifdef NOT_WALRUS
    auto docIDs = ReplicatorSGTest::getDocIDs(db);
    replParams.setDocIDs(docIDs);
#endif

    deleteAndRecreateDB();
    _encCBContext = NULL;
    TestEncryptorContext decryptContext = {doc2, "SSN"};
    _initParams.propertyDecryptor = &testDecryptorError;
    _decCBContext = &decryptContext;

    SECTION("LiteCoreDomain, kC4ErrorCrypto") {
        ExpectingExceptions x;
        decryptContext.simulateError = C4Error {LiteCoreDomain, kC4ErrorCrypto};
        _expectedDocPullErrors = { doc2 };
        replicate(replParams);
        CHECK(_callbackStatus.progress.documentCount == 2);
        CHECK(decryptContext.called == 1);

        // Try it again with good decryptor, but crypto errors will move the checkpoint
        // past the doc. The second attempt won't help.
        _initParams.propertyDecryptor = &testDecryptor;
        _expectedDocPullErrors = {};
        decryptContext.called = 0;
        replicate(replParams);
        CHECK(_callbackStatus.progress.documentCount == 0);
        CHECK(decryptContext.called == 0);
    }

    SECTION("WebSocketDomain/503") {
        ExpectingExceptions x;
        decryptContext.simulateError = C4Error {WebSocketDomain, 503};
        _mayGoOffline = true;
        _expectedDocPullErrorsAfterOffline = { doc2 };
        CHECK(decryptContext.called == 0);
        replicate(replParams);
        CHECK(_wentOffline);
        CHECK(decryptContext.called == 2);
        CHECK(c4db_getDocumentCount(db) == 3);

        // verify the content
        c4::ref<C4Document> doc = c4db_getDoc(db, slice(doc2), true, kDocGetAll, ERROR_INFO());
        REQUIRE(doc);
        Dict props = c4doc_getProperties(doc);
        CHECK(props.toJSON(false, true) == originalJSON);
    }
}
#endif //#ifdef COUCHBASE_ENTERPRISE

