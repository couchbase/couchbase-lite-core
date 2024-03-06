//
// ReplicatorSGTest.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ReplicatorAPITest.hh"
#include "CertHelper.hh"
#include "c4BlobStore.h"
#include "c4Collection.h"
#include "c4Document+Fleece.h"
#include "c4DocEnumerator.h"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "SecureRandomize.hh"
#include "fleece/Fleece.hh"
#include "SGTestUser.hh"
#include <cstdlib>
#include <array>

#ifndef _MSC_VER
#    include <unistd.h>
#endif

using namespace fleece;

constexpr size_t kDocBufSize = 20;

/* REAL-REPLICATOR (SYNC GATEWAY) TESTS

 The tests below are tagged [.SyncServer] to keep them from running during normal testing.
 Instead, they have to be invoked manually via the Catch command-line option `[.SyncServer]`.
 This is because they require that an external replication server is running.

 The default URL the tests connect to is blip://localhost:4984/scratch/, but this can be
 overridden by setting environment vars listed below.

 WARNING: The tests will erase the database named by REMOTE_DB (via the SG REST API.)

 Some tests connect to other databases by setting `_remoteDBName`. These have fixed contents.
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


class ReplicatorSGTest : public ReplicatorAPITest {
  public:
    ReplicatorSGTest() {
        if ( getenv("USE_CLIENT_CERT") ) {
#ifdef COUCHBASE_ENTERPRISE
            REQUIRE(Address::isSecure(_sg.address));
            Identity ca = CertHelper::readIdentity(sReplicatorFixturesDir + "ca_cert.pem",
                                                   sReplicatorFixturesDir + "ca_key.pem", "Couchbase");
            // The Common Name in the client cert has to be the email address of a user account
            // in Sync Gateway, or you only get guest access.
            Identity id =
                    CertHelper::createIdentity(false, kC4CertUsage_TLSClient, "Pupshaw", "pupshaw@couchbase.org", &ca);
            _sg.identityCert = id.cert;
            _sg.identityKey  = id.key;
#else
            FAIL("USE_CLIENT_CERT only works with EE builds");
#endif
        }
    }
};

TEST_CASE_METHOD(ReplicatorSGTest, "API Auth Failure", "[.SyncServer]") {
    _sg.remoteDBName = kProtectedDBName;

    SECTION("No Credentials") {}

    SECTION("Wrong Credentials") {
        Encoder enc;
        enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
        enc.beginDict();
        enc.writeKey(C4STR(kC4ReplicatorAuthType));
        enc.writeString("Basic"_sl);
        enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
        enc.writeString("brown");
        enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
        enc.writeString("sugar");
        enc.writeKey(C4STR(kC4ReplicatorAuthEnableChallengeAuth));

        SECTION("Preemptive Auth") { enc.writeBool(false); }

        SECTION("Challenge Auth") { enc.writeBool(true); }
        enc.endDict();
        enc.endDict();
        _options = AllocedDict(enc.finish());
    }

    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(_callbackStatus.error.domain == WebSocketDomain);
    CHECK(_callbackStatus.error.code == 401);
    CHECK(_headers["Www-Authenticate"].asString() == "Basic realm=\"Couchbase Sync Gateway\""_sl);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Auth Success", "[.SyncServer]") {
    _sg.remoteDBName = kProtectedDBName;

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
    enc.writeKey(C4STR(kC4ReplicatorAuthEnableChallengeAuth));

    SECTION("Preemptive Auth") { enc.writeBool(false); }

    SECTION("Challenge Auth") { enc.writeBool(true); }
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    replicate(kC4OneShot, kC4Disabled, true);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API ExtraHeaders", "[.SyncServer]") {
    _sg.remoteDBName = kProtectedDBName;

    // Use the extra-headers option to add HTTP Basic auth:
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionExtraHeaders));
    enc.beginDict();
    enc.writeKey("Authorization"_sl);
    enc.writeString("Basic cHVwc2hhdzpmcmFuaw=="_sl);  // that's user 'pupshaw', password 'frank'
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    replicate(kC4OneShot, kC4Disabled, true);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Push Empty DB", "[.SyncServer]") { replicate(kC4OneShot, kC4Disabled); }

TEST_CASE_METHOD(ReplicatorSGTest, "API Push Non-Empty DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Push Empty Doc", "[.SyncServer]") {
    Encoder enc;
    enc.beginDict();
    enc.endDict();
    alloc_slice body = enc.finish();
    createRev("doc"_sl, kRevID, body);

    replicate(kC4OneShot, kC4Disabled);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Push Big DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    replicate(kC4OneShot, kC4Disabled);
}


#if 0
TEST_CASE_METHOD(ReplicatorSGTest, "API Push Large-Docs DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json");
    replicate(kC4OneShot, kC4Disabled);
}
#endif


TEST_CASE_METHOD(ReplicatorSGTest, "API Push 5000 Changes", "[.SyncServer]") {
    std::string revID;
    {
        TransactionHelper t(db);
        revID = createNewRev(db, "Doc"_sl, nullslice, kFleeceBody);
    }
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Mutations --------");
    {
        TransactionHelper t(db);
        for ( int i = 2; i <= 5000; ++i ) revID = createNewRev(db, "Doc"_sl, slice(revID), kFleeceBody);
    }

    C4Log("-------- Second Replication --------");
    replicate(kC4OneShot, kC4Disabled);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Pull", "[.SyncServer]") {
    _sg.remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Pull With Indexes", "[.SyncServer]") {
    // Indexes slow down doc insertion, so they affect replicator performance.
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    REQUIRE(c4coll_createIndex(defaultColl, C4STR("Name"), C4STR("[[\".Name\"]]"), kC4JSONQuery, kC4FullTextIndex,
                               nullptr, nullptr));
    REQUIRE(c4coll_createIndex(defaultColl, C4STR("Artist"), C4STR("[[\".Artist\"]]"), kC4JSONQuery, kC4ValueIndex,
                               nullptr, nullptr));
    REQUIRE(c4coll_createIndex(defaultColl, C4STR("Year"), C4STR("[[\".Year\"]]"), kC4JSONQuery, kC4ValueIndex, nullptr,
                               nullptr));

    _sg.remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Continuous Push", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _stopWhenIdle = true;
    replicate(kC4Continuous, kC4Disabled);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Continuous Pull", "[.SyncServer]") {
    _sg.remoteDBName = kITunesDBName;
    _stopWhenIdle    = true;
    replicate(kC4Disabled, kC4Continuous);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Continuous Pull Forever", "[.SyncServer_Special]") {
    _sg.remoteDBName = kScratchDBName;
    _stopWhenIdle    = false;  // This test will NOT STOP ON ITS OWN
    _mayGoOffline    = true;
    replicate(kC4Disabled, kC4Continuous);
    // For CBL-2204: Wait for replicator to go idle, then shut down (Ctrl-C) SG process.
}

TEST_CASE_METHOD(ReplicatorSGTest, "Stop after Idle with Error", "[.SyncServer]") {
    // CBL-2501. This test is motivated by this bug. The bug bites when it finds a network error as the replicator
    // closes the socket after being stopped. Not able to find a way to inject the error, I tested
    // this case by tempering with the code in WebSocketImpl.onClose() and inject a transient error,
    // CloseStatus { kWebSocketClose, kCodeAbnormal }
    // Before the fix: continuous retry after Stopping;
    // after the fix: stop with the error regardless of it being transient.
    _sg.remoteDBName = kScratchDBName;
    _mayGoOffline    = true;
    _stopWhenIdle    = true;
    replicate(kC4Disabled, kC4Continuous, false);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Push & Pull Deletion", "[.SyncServer]") {
    createRev("doc"_sl, kRevID, kFleeceBody);
    createRev("doc"_sl, kRev2ID, kEmptyFleeceBody, kRevDeleted);

    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();
    createRev("doc"_sl, kRevID, kFleeceBody);

    replicate(kC4Disabled, kC4OneShot);

    auto                defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    c4::ref<C4Document> doc         = c4coll_getDoc(defaultColl, "doc"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc);

    CHECK(doc->revID == kRev2ID);
    CHECK((doc->flags & kDocDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == kRevID);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Push & Pull Attachments", "[.SyncServer]") {
    std::vector<std::string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    std::vector<C4BlobKey>   blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
    }

    C4Error             error;
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc         = c4coll_getDoc(defaultColl, "att1"_sl, true, kDocGetCurrentRev, ERROR_INFO(error));
    REQUIRE(doc);
    alloc_slice before = c4doc_bodyAsJSON(doc, true, ERROR_INFO(error));
    CHECK(before);
    C4Log("Original doc: %.*s", SPLAT(before));

    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();

    replicate(kC4Disabled, kC4OneShot);

    doc = c4coll_getDoc(defaultColl, "att1"_sl, true, kDocGetCurrentRev, ERROR_INFO(error));
    REQUIRE(doc);
    alloc_slice after = c4doc_bodyAsJSON(doc, true, ERROR_INFO(error));
    CHECK(after);
    C4Log("Pulled doc: %.*s", SPLAT(after));

    // Is the pulled identical to the original?
    CHECK(after == before);

    // Did we get all of its attachments?
    auto blobStore = c4db_getBlobStore(db, ERROR_INFO(error));
    REQUIRE(blobStore);
    for ( auto key : blobKeys ) {
        alloc_slice blob = c4blob_getContents(blobStore, key, ERROR_INFO(error));
        CHECK(blob);
    }
}

TEST_CASE_METHOD(ReplicatorSGTest, "Prove Attachments", "[.SyncServer]") {
    std::vector<std::string> attachments = {"Hey, this is an attachment!"};
    {
        TransactionHelper t(db);
        addDocWithAttachments("doc one"_sl, attachments, "text/plain");
    }
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Creating 2nd doc with same attachments --------");

    {
        TransactionHelper t(db);
        addDocWithAttachments("doc two"_sl, attachments, "text/plain");
    }
    // Pushing the second doc will cause Sync Gateway to ask for proof (send "proveAttachment")
    // instead of requesting the attachment itself, since it already has the attachment.
    replicate(kC4OneShot, kC4Disabled);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Pull Big Attachments", "[.SyncServer]") {
    _sg.remoteDBName = kImagesDBName;
    replicate(kC4Disabled, kC4OneShot);

    C4Error             error;
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc = c4coll_getDoc(defaultColl, "Abstract"_sl, true, kDocGetCurrentRev, ERROR_INFO(error));
    REQUIRE(doc);
    Dict root   = c4doc_getProperties(doc);
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

TEST_CASE_METHOD(ReplicatorSGTest, "API Push Conflict", "[.SyncServer]") {
    const std::string originalRevID = "1-3cb9cfb09f3f0b5142e618553966ab73539b8888";
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);

    _sg.upsertDoc(kC4DefaultCollectionSpec, "0000013",
                  R"({"_rev":")" + originalRevID + R"(","serverSideUpdate":true})");

    createRev("0000013"_sl, "2-f000"_sl, kFleeceBody);

    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc         = c4coll_getDoc(defaultColl, C4STR("0000013"), true, kDocGetAll, nullptr);
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
    _expectedDocPushErrors = {"0000013"};
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Pulling --------");
    _expectedDocPushErrors = {};
    _expectedDocPullErrors = {"0000013"};
    replicate(kC4Disabled, kC4OneShot);

    C4Log("-------- Checking Conflict --------");
    doc = c4coll_getDoc(defaultColl, C4STR("0000013"), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK((doc->flags & kDocConflicted) != 0);
    revID = C4STR("2-f000");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(c4doc_getProperties(doc) != nullptr);
    REQUIRE(c4doc_selectParentRevision(doc));
    revID = slice(originalRevID);
    CHECK(doc->selectedRev.revID == revID);
#if 0  // FIX: These checks fail due to issue #402; re-enable when fixing that bug
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

TEST_CASE_METHOD(ReplicatorSGTest, "Update Once-Conflicted Doc", "[.SyncServer]") {
    // For issue #448.
    // Create a conflicted doc on SG, and resolve the conflict:
    _sg.remoteDBName = "scratch_allows_conflicts"_sl;
    flushScratchDatabase();

    const std::array<std::string, 4> bodies = {R"({"_rev":"1-aaaa","foo":1})",
                                               R"({"_revisions":{"start":2,"ids":["bbbb","aaaa"]},"foo":2.1})",
                                               R"({"_revisions":{"start":2,"ids":["cccc","aaaa"]},"foo":2.2})",
                                               R"({"_revisions":{"start":3,"ids":["dddd","cccc"]},"_deleted":true})"};

    for ( const std::string& body : bodies ) {
        _sg.upsertDoc(kC4DefaultCollectionSpec, "doc?new_edits=false", slice(body));
    }

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4OneShot, kC4OneShot);

    // Verify doc:
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc         = c4coll_getDoc(defaultColl, "doc"_sl, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    C4Slice revID = C4STR("2-bbbb");
    CHECK(doc->revID == revID);
    CHECK((doc->flags & kDocDeleted) == 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == "1-aaaa"_sl);

    // Update doc:
    createRev("doc"_sl, "3-ffff"_sl, kFleeceBody);

    // Push change back to SG:
    C4Log("-------- Pushing");
    replicate(kC4OneShot, kC4OneShot);

    // Verify doc is updated on SG:
    auto    body      = _sg.getDoc("doc", kC4DefaultCollectionSpec);
    C4Slice bodySlice = C4STR("{\"_id\":\"doc\",\"_rev\":\"3-ffff\",\"ans*wer\":42}");
    CHECK(C4Slice(body) == bodySlice);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Pull multiply-updated", "[.SyncServer]") {
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

    flushScratchDatabase();
    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc?new_edits=false", R"({"count":1, "_rev":"1-1111"})");

    replicate(kC4Disabled, kC4OneShot);
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc         = c4coll_getDoc(defaultColl, "doc"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "1-1111"_sl);

    const std::array<std::string, 3> bodies = {R"({"count":2, "_rev":"1-1111"})",
                                               R"({"count":3, "_rev":"2-c5557c751fcbfe4cd1f7221085d9ff70"})",
                                               R"({"count":4, "_rev":"3-2284e35327a3628df1ca8161edc78999"})"};

    for ( const std::string& body : bodies ) { _sg.upsertDoc(kC4DefaultCollectionSpec, "doc", slice(body)); }

    replicate(kC4Disabled, kC4OneShot);
    doc = c4coll_getDoc(defaultColl, "doc"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc);
    CHECK(doc->revID == "4-ffa3011c5ade4ec3a3ec5fe2296605ce"_sl);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Pull deltas from SG", "[.SyncServer][Delta]") {
    static constexpr int kNumDocs = 1000, kNumProps = 1000;
    flushScratchDatabase();
    auto defaultColl = c4db_getDefaultCollection(db, nullptr);

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        std::srand(123456);  // start random() sequence at a known place
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "doc-%03d", docNo);
            Encoder enc(c4db_createFleeceEncoder(db));
            enc.beginDict();
            for ( int p = 0; p < kNumProps; ++p ) {
                enc.writeKey(format("field%03d", p));
                enc.writeInt(std::rand());
            }
            enc.endDict();
            alloc_slice body  = enc.finish();
            std::string revID = createNewRev(db, slice(docID), body);
        }
    };
    populateDB();

    C4Log("-------- Pushing to SG --------");
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    {
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "doc-%03d", docNo);
            C4Error             error;
            c4::ref<C4Document> doc =
                    c4coll_getDoc(defaultColl, slice(docID), false, kDocGetCurrentRev, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for ( Dict::iterator i(props); i; ++i ) {
                enc.writeKey(i.keyString());
                auto value = i.value().asInt();
                if ( RandomNumber() % 8 == 0 ) value = RandomNumber();
                enc.writeInt(value);
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        _sg.insertBulkDocs(kC4DefaultCollectionSpec, enc.finish());
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for ( int pass = 1; pass <= 3; ++pass ) {
        if ( pass == 3 ) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            Encoder enc;
            enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorOptionDisableDeltas));
            enc.writeBool(true);
            enc.endDict();
            _options = AllocedDict(enc.finish());
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDB();
        defaultColl = c4db_getDefaultCollection(db, nullptr);
        populateDB();
        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;
        replicate(kC4Disabled, kC4OneShot);
        double time = st.elapsed();
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, kNumDocs / time);
        if ( pass == 2 ) timeWithDelta = time;
        else if ( pass == 3 )
            timeWithoutDelta = time;
        int                      n = 0;
        C4Error                  error;
        c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(defaultColl, nullptr, ERROR_INFO(error));
        REQUIRE(e);
        while ( c4enum_next(e, ERROR_INFO(error)) ) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            CHECK(slice(info.docID).hasPrefix("doc-"_sl));
            CHECK(slice(info.revID).hasPrefix("2-"_sl));
            ++n;
        }
        CHECK(error.code == 0);
        CHECK(n == kNumDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed", timeWithDelta, timeWithoutDelta,
          timeWithoutDelta / timeWithDelta);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Pull iTunes deltas from SG", "[.SyncServer][Delta]") {
    flushScratchDatabase();

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    };
    populateDB();
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    auto numDocs     = c4coll_getDocumentCount(defaultColl);

    C4Log("-------- Pushing to SG --------");
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    {
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("docs"_sl);
        enc.beginArray();
        for ( int docNo = 0; docNo < numDocs; ++docNo ) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%07u", docNo + 1);
            C4Error             error;
            c4::ref<C4Document> doc =
                    c4coll_getDoc(defaultColl, slice(docID), false, kDocGetCurrentRev, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            enc.beginDict();
            enc.writeKey("_id"_sl);
            enc.writeString(docID);
            enc.writeKey("_rev"_sl);
            enc.writeString(doc->revID);
            for ( Dict::iterator i(props); i; ++i ) {
                enc.writeKey(i.keyString());
                auto value = i.value();
                if ( i.keyString() == "Play Count"_sl ) enc.writeInt(value.asInt() + 1);
                else
                    enc.writeValue(value);
            }
            enc.endDict();
        }
        enc.endArray();
        enc.endDict();
        _sg.insertBulkDocs(kC4DefaultCollectionSpec, enc.finish());
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for ( int pass = 1; pass <= 3; ++pass ) {
        if ( pass == 3 ) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            Encoder enc;
            enc.beginDict();
            enc.writeKey(C4STR(kC4ReplicatorOptionDisableDeltas));
            enc.writeBool(true);
            enc.endDict();
            _options = AllocedDict(enc.finish());
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);
        deleteAndRecreateDB();
        populateDB();
        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;
        replicate(kC4Disabled, kC4OneShot);
        double time = st.elapsed();
        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, numDocs / time);
        if ( pass == 2 ) timeWithDelta = time;
        else if ( pass == 3 )
            timeWithoutDelta = time;

        int                      n = 0;
        C4Error                  error;
        c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(defaultColl, nullptr, ERROR_INFO(error));
        REQUIRE(e);
        while ( c4enum_next(e, ERROR_INFO(error)) ) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            //CHECK(slice(info.docID).hasPrefix("doc-"_sl));
            CHECK(slice(info.revID).hasPrefix("2-"_sl));
            ++n;
        }
        CHECK(error.code == 0);
        CHECK(n == numDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed", timeWithDelta, timeWithoutDelta,
          timeWithoutDelta / timeWithDelta);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Replicator count balance", "[.SyncServer]") {
    flushScratchDatabase();
    bool logRemoteRequests = false;

    C4Log("-------- Populating local db --------");
    size_t numDocs    = 100;
    auto   populateDB = [&]() {
        TransactionHelper t(db);
        importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", 0.0, false, nullptr, (size_t)numDocs);
    };
    populateDB();
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    REQUIRE(c4coll_getDocumentCount(defaultColl) == numDocs);

    C4Log("-------- Pushing to SG --------");
    replicate(kC4OneShot, kC4Disabled);

    std::vector<std::vector<alloc_slice>> revIDs;
    std::vector<std::string>              docIDs;
    revIDs.emplace_back();
    for ( int docNo = 0; docNo < numDocs; ++docNo ) {
        char buf[kDocBufSize];
        snprintf(buf, kDocBufSize, "%07u", docNo + 1);
        docIDs.emplace_back(buf);
        c4::ref<C4Document> doc = c4coll_getDoc(defaultColl, slice(buf), true, kDocGetCurrentRev, ERROR_INFO());
        REQUIRE(doc);
        revIDs.back().emplace_back(doc->revID);
    }

    C4Log("-------- Updating docs on SG --------");
    // Now update the docs on SG:
    unsigned    numUpdates = 14;
    std::thread th([&]() {
        unsigned nu = numUpdates;
        for ( int c = 1; c <= nu; ++c ) {
            revIDs.emplace_back();
            for ( int docNo = 0; docNo < numDocs; ++docNo ) {
                const char*         docID = docIDs[docNo].c_str();
                c4::ref<C4Document> doc =
                        c4coll_getDoc(defaultColl, slice(docID), true, kDocGetCurrentRev, ERROR_INFO());
                REQUIRE(doc);

                Dict        props = c4doc_getProperties(doc);
                JSONEncoder enc;
                enc.beginDict();
                enc.writeKey("_id"_sl);
                enc.writeString(docID);
                enc.writeKey("_rev"_sl);
                enc.writeString(revIDs[c - 1][docNo]);
                for ( Dict::iterator i(props); i; ++i ) {
                    enc.writeKey(i.keyString());
                    auto value = i.value();
                    if ( i.keyString() == "Total Time"_sl ) enc.writeInt(100 + c);
                    else
                        enc.writeValue(value);
                }
                enc.endDict();

                FLError     flError;
                alloc_slice res =
                        _sg.sendRemoteRequest("PUT", docID, enc.finish(), false, HTTPStatus::OK, logRemoteRequests);
                fleece::Doc fdoc = Doc::fromJSON(res, &flError);
                REQUIRE(flError == kFLNoError);
                Dict resDict = fdoc.root().asDict();
                revIDs[c].emplace_back(resDict.get(Dict::Key("rev"_sl)).asString());
            }
        }
    });
    th.detach();

    REQUIRE(startReplicator(kC4Continuous, kC4Continuous, WITH_ERROR()));

    // Wait for Idle state
    while ( c4repl_getStatus(_repl).level != kC4Idle ) { std::this_thread::sleep_for(1ms); }
    // we wait for all documents to reach revision numUpdates + 1.
    while ( true ) {
        bool done = true;
        for ( slice docId : docIDs ) {
            c4::ref<C4Document> doc   = c4coll_getDoc(defaultColl, docId, true, kDocGetCurrentRev, ERROR_INFO());
            std::string         revid = std::string(doc->revID);
            std::istringstream  is(revid);
            int                 i;
            is >> i;
            if ( i <= numUpdates ) {
                done = false;
                std::cout << "Replicator comes to Idle but not done" << std::endl;
                std::this_thread::sleep_for(1ms);
                break;
            }
        }
        if ( done ) { break; }
    }
    // All documents reached target revisions. Now, stop it.
    c4repl_stop(_repl);
    C4ReplicatorStatus status;
    while ( (status = c4repl_getStatus(_repl)).level != kC4Stopped ) { std::this_thread::sleep_for(1ms); }

    C4Log("-------- status.total=%lld, status.completed=%lld, status.docCount=%lld, number of documents in the "
          "database=%lld\n",
          status.progress.unitsTotal, status.progress.unitsCompleted, status.progress.documentCount,
          c4db_getDocumentCount(db));

    CHECK(status.progress.unitsTotal == status.progress.unitsCompleted);
}

// This test requires SG 3.0
TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Enabled - Revoke Access", "[.SyncServer]") {
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
    if ( !requireSG3() ) return;  // skip test unless SG is ≥ 3.0

    const std::string              channelIDa = "a";
    const std::string              channelIDb = "b";
    const std::vector<std::string> channelIDs = {channelIDa, channelIDb};

    // Create docs on SG:
    SG::TestUser testUser{_sg, "apera", channelIDs};
    _sg.authHeader = testUser.authHeader();

    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc1", "{}", channelIDs);

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorAuthType));
    enc.writeString("Basic"_sl);
    enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
    enc.writeString(testUser._username);
    enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
    enc.writeString(testUser._password);
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) {
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
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc1        = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("1-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoked access to channel 'a':
    testUser.setChannels({channelIDb});

    // Check if update to doc1 is still pullable:
    auto oRevID = slice(doc1->revID).asString();
    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc1", R"({"_rev":")" + oRevID + "\"}", {channelIDb});

    C4Log("-------- Pull update");
    replicate(kC4Disabled, kC4OneShot);

    // Verify the update:
    doc1 = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(kC4Disabled, kC4OneShot);

    // Verify if doc1 is purged:
    doc1 = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(!doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}

// This test requires SG 3.0
TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Enabled - Filter Revoked Revision", "[.SyncServer]") {
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
    if ( !requireSG3() ) return;  // skip test unless SG is ≥ 3.0

    const std::string channelID = "a";

    // Create temp user for test
    SG::TestUser testUser{_sg, "apefrr", {channelID}};
    _sg.authHeader = testUser.authHeader();
    // Create doc on SG
    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc1", "{}", {channelID});

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorAuthType));
    enc.writeString("Basic"_sl);
    enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
    enc.writeString(testUser._username);
    enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
    enc.writeString(testUser._password);
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    // Setup pull filter to filter the _removed rev:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) {
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
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc1        = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(kC4Disabled, kC4OneShot);

    // Verify if doc1 is not purged as the revoked rev is filtered:
    doc1 = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}

// This test requires SG 3.0
TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Disabled - Revoke Access", "[.SyncServer]") {
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();
    if ( !requireSG3() ) return;  // skip test unless SG is ≥ 3.0

    const std::string channelID = "a";

    // Create temp user for test
    SG::TestUser testUser{_sg, "apdra", {channelID}};
    _sg.authHeader = testUser.authHeader();
    // Create doc on SG
    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc1", "{}", {channelID});

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
    enc.writeString(testUser._username);
    enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
    enc.writeString(testUser._password);
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_counter++; }
        return true;
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4Disabled, kC4OneShot);

    // Verify:
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc1        = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(testUser.revokeAllChannels());

    C4Log("-------- Pulling the revoked");
    replicate(kC4Disabled, kC4OneShot);

    // Verify if the doc1 is not purged as the auto purge is disabled:
    doc1 = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    // No pull filter called
    CHECK(_counter == 0);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Enabled - Remove Doc From Channel", "[.SyncServer]") {
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();

    const std::string              channelIDa = "a";
    const std::string              channelIDb = "b";
    const std::vector<std::string> channelIDs = {channelIDa, channelIDb};

    // Create temp user for test
    SG::TestUser testUser{_sg, "aperdfc", channelIDs};
    _sg.authHeader = testUser.authHeader();
    // Create doc on SG
    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc1", "{}", channelIDs);

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorAuthType));
    enc.writeString("Basic"_sl);
    enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
    enc.writeString(testUser._username);
    enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
    enc.writeString(testUser._password);
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) {
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
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc1        = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("1-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Removed doc from channel 'a':
    auto oRevID = slice(doc1->revID).asString();
    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc1", R"({"_rev":")" + oRevID + "\"}", {channelIDb});

    C4Log("-------- Pull update");
    replicate(kC4Disabled, kC4OneShot);

    // Verify the update:
    doc1 = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Remove doc from all channels:
    oRevID = slice(doc1->revID).asString();
    _sg.upsertDocWithEmptyChannels(kC4DefaultCollectionSpec, "doc1", R"({"_rev":")" + oRevID + "\"}");

    C4Log("-------- Pull the removed");
    replicate(kC4Disabled, kC4OneShot);

    // Verify if doc1 is purged:
    doc1 = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(!doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Enabled - Filter Removed Revision", "[.SyncServer]") {
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();

    const std::string channelID = "a";

    // Create temp user for test
    SG::TestUser testUser{_sg, "apefrr", {channelID}};
    _sg.authHeader = testUser.authHeader();
    // Create docs on SG:
    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc1", "{}", {channelID});

    // Setup Replicator Options:
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionAuthentication));
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorAuthType));
    enc.writeString("Basic"_sl);
    enc.writeKey(C4STR(kC4ReplicatorAuthUserName));
    enc.writeString(testUser._username);
    enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
    enc.writeString(testUser._password);
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    // Setup pull filter to filter the _removed rev:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) {
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
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc1        = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Remove doc from all channels
    auto oRevID = slice(doc1->revID).asString();
    _sg.upsertDocWithEmptyChannels(kC4DefaultCollectionSpec, "doc1", R"({"_rev":")" + oRevID + "\"}");

    C4Log("-------- Pull the removed");
    replicate(kC4Disabled, kC4OneShot);

    // Verify if doc1 is not purged as the removed rev is filtered:
    doc1 = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Disabled - Remove Doc From Channel", "[.SyncServer]") {
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();

    const std::string channelID = "a";

    // Create temp user for test
    SG::TestUser testUser{_sg, "apdrdfc", {channelID}};
    _sg.authHeader = testUser.authHeader();

    // Create docs on SG:
    _sg.upsertDoc(kC4DefaultCollectionSpec, "doc1", "{}", {channelID});

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
    enc.writeString(testUser._username);
    enc.writeKey(C4STR(kC4ReplicatorAuthPassword));
    enc.writeString(testUser._password);
    enc.endDict();
    enc.endDict();
    _options = AllocedDict(enc.finish());

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_counter++; }
        return true;
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4Disabled, kC4OneShot);

    // Verify:
    auto                defaultColl = c4db_getDefaultCollection(db, nullptr);
    c4::ref<C4Document> doc1        = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Remove doc from all channels
    auto oRevID = slice(doc1->revID).asString();
    _sg.upsertDocWithEmptyChannels(kC4DefaultCollectionSpec, "doc1", R"({"_rev":")" + oRevID + "\"}");

    C4Log("-------- Pulling the removed");
    replicate(kC4Disabled, kC4OneShot);

    // Verify if the doc1 is not purged as the auto purge is disabled:
    doc1 = c4coll_getDoc(defaultColl, "doc1"_sl, true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    // No pull filter called
    CHECK(_counter == 0);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Enabled(default) - Delete Doc", "[.SyncServer]") {
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();

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
    _options         = AllocedDict(enc.finish());
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    // Create a doc and push it:
    c4::ref<C4Document> doc;
    FLSlice             docID = C4STR("doc");
    {
        TransactionHelper t(db);
        C4Error           error;
        doc = c4coll_createDoc(defaultColl, docID, json2fleece("{channels:['a']}"), 0, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
    }
    CHECK(c4coll_getDocumentCount(defaultColl) == 1);
    replicate(kC4OneShot, kC4Disabled);

    // Delete the doc and push it:
    {
        TransactionHelper t(db);
        C4Error           error;
        doc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
        REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    }
    CHECK(c4coll_getDocumentCount(defaultColl) == 0);
    replicate(kC4OneShot, kC4Disabled);

    // Apply a pull and verify that the document is not purged.
    replicate(kC4Disabled, kC4OneShot);
    C4Error error;
    doc = c4coll_getDoc(defaultColl, C4STR("doc"), true, kDocGetAll, ERROR_INFO(error));
    CHECK(error.code == 0);
    CHECK(doc != nullptr);
    REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    CHECK(c4coll_getDocumentCount(defaultColl) == 0);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Enabled(default) - Delete then Create Doc", "[.SyncServer]") {
    _sg.remoteDBName = "scratch_revocation"_sl;
    flushScratchDatabase();

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
    _options         = AllocedDict(enc.finish());
    auto defaultColl = getCollection(db, kC4DefaultCollectionSpec);
    // Create a new doc and push it:
    c4::ref<C4Document> doc;
    FLSlice             docID = C4STR("doc");
    {
        TransactionHelper t(db);
        C4Error           error;
        doc = c4coll_createDoc(defaultColl, docID, json2fleece("{channels:['a']}"), 0, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
    }
    CHECK(c4coll_getDocumentCount(defaultColl) == 1);
    replicate(kC4OneShot, kC4Disabled);

    // Delete the doc and push it:
    {
        TransactionHelper t(db);
        C4Error           error;
        doc = c4doc_update(doc, kC4SliceNull, kRevDeleted, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
        REQUIRE(doc->flags == (C4DocumentFlags)(kDocExists | kDocDeleted));
    }
    CHECK(c4coll_getDocumentCount(defaultColl) == 0);
    replicate(kC4OneShot, kC4Disabled);

    // Create a new doc with the same id that was deleted:
    {
        TransactionHelper t(db);
        C4Error           error;
        doc = c4coll_createDoc(defaultColl, docID, json2fleece("{channels:['a']}"), 0, ERROR_INFO(error));
        CHECK(error.code == 0);
        REQUIRE(doc);
    }
    CHECK(c4coll_getDocumentCount(defaultColl) == 1);

    // Apply a pull and verify the document is not purged:
    replicate(kC4Disabled, kC4OneShot);
    C4Error             error;
    c4::ref<C4Document> doc2 = c4coll_getDoc(defaultColl, docID, true, kDocGetAll, ERROR_INFO(error));
    CHECK(error.code == 0);
    CHECK(doc2 != nullptr);
    CHECK(c4coll_getDocumentCount(defaultColl) == 1);
    CHECK(doc2->revID == doc->revID);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Pinned Certificate Failure", "[.SyncServer]") {
    if ( !Address::isSecure(_sg.address) ) { return; }
    flushScratchDatabase();

    // Using an unmatched pinned cert:
    _sg.pinnedCert = "-----BEGIN CERTIFICATE-----\r\n"
                     "MIICpDCCAYwCCQCskbhc/nbA5jANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\r\n"
                     "b2NhbGhvc3QwHhcNMjIwNDA4MDEwNDE1WhcNMzIwNDA1MDEwNDE1WjAUMRIwEAYD\r\n"
                     "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDQ\r\n"
                     "vl0M5D7ZglW76p428x7iQoSkhNyRBEjZgSqvQW3jAIsIElWu7mVIIAm1tpZ5i5+Q\r\n"
                     "CHnFLha1TDACb0MUa1knnGj/8EsdOADvBfdBq7AotypiqBayRUNdZmLoQEhDDsen\r\n"
                     "pEHMDmBrDsWrgNG82OMFHmjK+x0RioYTOlvBbqMAX8Nqp6Yu/9N2vW7YBZ5ovsr7\r\n"
                     "vdFJkSgUYXID9zw/MN4asBQPqMT6jMwlxR1bPqjsNgXrMOaFHT/2xXdfCvq2TBXu\r\n"
                     "H7evR6F7ayNcMReeMPuLOSWxA6Fefp8L4yDMW23jizNIGN122BgJXTyLXFtvg7CQ\r\n"
                     "tMnE7k07LLYg3LcIeamrAgMBAAEwDQYJKoZIhvcNAQELBQADggEBABdQVNSIWcDS\r\n"
                     "sDPXk9ZMY3stY9wj7VZF7IO1V57n+JYV1tJsyU7HZPgSle5oGTSkB2Dj1oBuPqnd\r\n"
                     "8XTS/b956hdrqmzxNii8sGcHvWWaZhHrh7Wqa5EceJrnyVM/Q4uoSbOJhLntLE+a\r\n"
                     "FeFLQkPpJxdtjEUHSAB9K9zCO92UC/+mBUelHgztsTl+PvnRRGC+YdLy521ST8BI\r\n"
                     "luKJ3JANncQ4pCTrobH/EuC46ola0fxF8G5LuP+kEpLAh2y2nuB+FWoUatN5FQxa\r\n"
                     "+4F330aYRvDKDf8r+ve3DtchkUpV9Xa1kcDFyTcYGKBrINtjRmCIblA1fezw59ZT\r\n"
                     "S5TnM2/TjtQ=\r\n"
                     "-----END CERTIFICATE-----\r\n";

    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUntrusted);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Pinned Certificate Success", "[.SyncServer]") {
    if ( !Address::isSecure(_sg.address) ) { return; }
    flushScratchDatabase();

    // Leaf:
    _sg.pinnedCert = "-----BEGIN CERTIFICATE-----\r\n"
                     "MIICoDCCAYgCCQDOqeOThcl0DTANBgkqhkiG9w0BAQsFADAQMQ4wDAYDVQQDDAVJ\r\n"
                     "bnRlcjAeFw0yMjA0MDgwNDE2MjNaFw0zMjA0MDUwNDE2MjNaMBQxEjAQBgNVBAMM\r\n"
                     "CWxvY2FsaG9zdDCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMt7VQ0j\r\n"
                     "74/GJVnTfC0YQZHeCFoZbZyJ/4KPOpe1UoqRQ1xNtllPMHf4ukIeNd3tS4CHQDqK\r\n"
                     "83a7uGXEOzY3JFaVRnTpMcHRMnpmZQLWZs+WMCP5fzI4EcaJjFmqQSUjfZiocdh/\r\n"
                     "n5vKc64bhKyUStE2CSObMnJ/L5mPY1JUAgxQrXtK4lw1T/ppV2m4hiutr+gkhXjc\r\n"
                     "Sam4DheuMg7hSUZSwh7VI253ev1Hp4JdSmndQHvle99S+N5jJ11NZnEuQxcImmOI\r\n"
                     "MBVfRFpREFPOH+JrqsnYSic2GQvv31nAJsXzYX2t/VT0a3TUes3B9OZfAVA7nMFA\r\n"
                     "r3E9mjVGYVtn7skCAwEAATANBgkqhkiG9w0BAQsFAAOCAQEADbjYO9VxOGZT5LAv\r\n"
                     "ON+U+2FPG5Tons1ubWslThROqml7CCfNKPVhZCwe0BUQLWc35NYvqVjoSAenCHu6\r\n"
                     "EUANfqtuNxQAoeDCaP1epGYZ8fakJXvuyTjek3RV2PeiuFUIZQP/HWGfI640kh4V\r\n"
                     "xvUBa3joelnt+KjDB/yJemmf0dIXJ0dLtFBTN+YVp4aSFTtzcbqh50H6BSAgSiWR\r\n"
                     "ocTu5YpDXHZ6ufaMTRa2HUcSmFeWi75sS6ySgECTbeld1/mFZcSf1zXHU9WFg39D\r\n"
                     "knQNR2i1cJMbMZ3GCRyB6y3SxFb7/9BS70DV3p4n5BjYMlhNnHJx4u1JUTLWgybV\r\n"
                     "qrV+HA==\r\n"
                     "-----END CERTIFICATE-----\r\n";
    replicate(kC4OneShot, kC4Disabled, true);

    // Intermediate:
    _sg.pinnedCert = "-----BEGIN CERTIFICATE-----\r\n"
                     "MIIDFTCCAf2gAwIBAgIJANZ8gSANI5jNMA0GCSqGSIb3DQEBCwUAMA8xDTALBgNV\r\n"
                     "BAMMBFJvb3QwHhcNMjIwNDA4MDQxNjIzWhcNMzIwNDA1MDQxNjIzWjAQMQ4wDAYD\r\n"
                     "VQQDDAVJbnRlcjCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAOm1MUNQ\r\n"
                     "xZKOCXw93eB/pmyCk5kEV3+H8RQC5Nq7orHvnHL6D/YVfVsobZyHkMSP3FVzl0bo\r\n"
                     "s1s+8kCjJ7O+M3TpzuSL8y4uLSEPmZF5qY2N7QobabrKVYueFxFmOD7+ypILx2QC\r\n"
                     "+hWd3J3XiLiiXqOO2jtjtwwy2+pD21DjmcPHGC4GKyv8/jp7hH4MFF6ux1wRQej1\r\n"
                     "on5jJQNFERUFdfX3wAmZgjww8bfyCEkHxnyIfJjEhyOtMLGGNUu8Hms7az+uYT6I\r\n"
                     "S4Q6VeBJ5WTKyhk7aJB1Rl6zZbROvTIq+ZaxAJNwsIzd/HiaoTwFUe3EFilIeGFK\r\n"
                     "w3vnPwiq99tDBHsCAwEAAaNzMHEwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQU\r\n"
                     "WXW5x/ufCrRKhv3F5wBqY0JVUEswPwYDVR0jBDgwNoAUefIiQi9GC9aBspej7UJT\r\n"
                     "zQzs/mKhE6QRMA8xDTALBgNVBAMMBFJvb3SCCQD1tOzs5zPQ/zANBgkqhkiG9w0B\r\n"
                     "AQsFAAOCAQEAEJhO1fA0d8Hu/5IHTlsGfmtcXOyXDcQQVz/3FKWrTPgDOYeMMNbG\r\n"
                     "WqvuG4YxmXt/+2OC1IYK/slrIK5XXldfRu90UM4wVXeD3ATLS3AG0Z/+yPRGbUbF\r\n"
                     "y5+11nXySGyKdV1ik0KgLGeYf0cuJ/vu+/7mkj4mGDfmTQv+8/HYKNaOqgKuVRlf\r\n"
                     "LHBh/RlbHMBn2nwL79vbrIeDaQ0zq9srt9F3CEy+SvlxX63Txmrym3fqTQjPUi5s\r\n"
                     "rEsy+eNr4N+aDWqGRcUkbP/C/ktGGNBHYG1NaPJq7CV1tdLe+usIcRWRR9vOBWbr\r\n"
                     "EkBGJMvCdhlWRv2FnrQ+VUQ+mhYHBS2Kng==\r\n"
                     "-----END CERTIFICATE-----\r\n";
    replicate(kC4OneShot, kC4Disabled, true);

    // Root:
    _sg.pinnedCert = "-----BEGIN CERTIFICATE-----\r\n"
                     "MIIDFDCCAfygAwIBAgIJAPW07OznM9D/MA0GCSqGSIb3DQEBCwUAMA8xDTALBgNV\r\n"
                     "BAMMBFJvb3QwHhcNMjIwNDA4MDQxNjIzWhcNMzIwNDA1MDQxNjIzWjAPMQ0wCwYD\r\n"
                     "VQQDDARSb290MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAvJV+Ptou\r\n"
                     "R1BS/0XXN+JImdNesaBJ2tcHrFHq2yK9V4qu2iUX8LgOcBpPg8yR0zJlzjwF+SLE\r\n"
                     "R8jBhD79YF8kF+r7cqBhsvy+e/ri0AaBiGsdP7NFPFEUCOukhnMIvLt10BvsRoCd\r\n"
                     "+eFrDZO0ZJer3ylp2GeB01rTgngWfrenhZdyGR8ISn+ijtN+J2IhAxsoLGDWiAL/\r\n"
                     "XWX55agSuAGi6zlomkReTMuyfkidLfrejUQCnrcDQQ7xqjdCB1QYBt6o1U1oHN3F\r\n"
                     "D6ICXirXJyVDJ2Ry6q+FrGJbJDUPlNwlPqAyukFFbeOINPKWiFQUw8nSo3i3DFMG\r\n"
                     "UZ3HhkQ/xfboZQIDAQABo3MwcTAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5\r\n"
                     "8iJCL0YL1oGyl6PtQlPNDOz+YjA/BgNVHSMEODA2gBR58iJCL0YL1oGyl6PtQlPN\r\n"
                     "DOz+YqETpBEwDzENMAsGA1UEAwwEUm9vdIIJAPW07OznM9D/MA0GCSqGSIb3DQEB\r\n"
                     "CwUAA4IBAQANxGwoeEBaibMQAqSWPnDBISiwk9uKy3buateXOtLlBSpM9ohE4iPG\r\n"
                     "GDFZ+9LoKJGy4vWmv6XD4zBeoqZ9hOgnvdEu0P+JITffjXCsfb0JPsOOjwbcJ+5+\r\n"
                     "TnfoXCyPRTEi/6OG1sKO2ibav5vMTUuUDdVYbPA2hfEAdn/n0GrN4fQ1USMKk+Ld\r\n"
                     "KWgWGZto+l0fKIXdHHpxr01V9Q/+6kzbpZOSxw41m/o1TwJxYSuRXZfK67YpBYGO\r\n"
                     "N4X2c7Qsvjd52vcZdRra+bkS0BJXwEDZZdmrZOlRAYIhE7lZ5ojqcZ+/UJztyPZq\r\n"
                     "Dbr9kMLDVeMuJfGyebdZ0zeMhVSv0PlD\r\n"
                     "-----END CERTIFICATE-----\r\n";
    replicate(kC4OneShot, kC4Disabled, true);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Set Network Interface", "[.SyncServer]") {
    if ( slice(_sg.address.hostname) != "localhost" ) return;

    // Disable Retries:
    {
        Encoder enc;
        enc.beginDict();
        enc[kC4ReplicatorOptionMaxRetries] = 0;
        enc.endDict();
        _options = AllocedDict(enc.finish());
    }

#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
    int code = 0;
#else
    int code = ENOTSUP;
#endif

    C4ErrorDomain domain = POSIXDomain;
    _sg.networkInterface = nullslice;

    SECTION("Reachable - Name") {
        // Use loopback interface connecting to localhost:
#if defined(__APPLE__)
        _sg.networkInterface = "lo0"_sl;
#elif defined(__linux__)
        _sg.networkInterface = "lo"_sl;
#elif defined(_WIN32)
        _sg.networkInterface = "Loopback Pseudo-Interface 1"_sl;
#else
        _sg.networkInterface = "lo0"_sl;
#endif
    }

    SECTION("Reachable - IP Address") {
        // Use loopback interface connecting to localhost:
        _sg.networkInterface = "127.0.0.1"_sl;
    }

    SECTION("Unreachable") {
        // Use ethernet interface connecting to localhost:
#if defined(__APPLE__)
        _sg.networkInterface = "en0"_sl;
        code                 = EADDRNOTAVAIL;
#elif defined(__linux__)
        _sg.networkInterface = "eth0"_sl;
        code                 = ETIMEDOUT;
#elif defined(_WIN32)
        // Note: Required Wi-Fi interface on the test machine.
        _sg.networkInterface = "Wi-Fi"_sl;
        code                 = EADDRNOTAVAIL;
#else
        _sg.networkInterface = "eth0"_sl;
#endif
    }

    bool success = (code == 0);
    replicate(kC4OneShot, kC4Disabled, success);
    if ( !success ) {
        CHECK(_callbackStatus.error.domain == domain);
        CHECK(_callbackStatus.error.code == code);
    }
}

TEST_CASE_METHOD(ReplicatorSGTest, "Set Invalid Network Interface", "[.SyncServer]") {
    _sg.networkInterface = "x0"_sl;
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(_callbackStatus.error.domain == POSIXDomain);
    CHECK(_callbackStatus.error.code == ENXIO);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Remove Attachment in SGW", "[.SyncServer]") {
    std::vector<std::string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    std::vector<C4BlobKey>   blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
    }

    C4Error             error;
    c4::ref<C4Document> doc = c4doc_get(db, "att1"_sl, true, ERROR_INFO(error));
    REQUIRE(doc);
    alloc_slice before = c4doc_bodyAsJSON(doc, true, ERROR_INFO(error));
    CHECK(before);
    doc = nullptr;
    C4Log("Original doc: %.*s", SPLAT(before));

    replicate(kC4OneShot, kC4Disabled);

    HTTPStatus  status;
    alloc_slice result = _sg.sendRemoteRequest("GET", "/scratch/att1", &status, &error);
    REQUIRE(status == HTTPStatus::OK);

    FLError     flError = kFLNoError;
    MutableDict rev1    = FLMutableDict_NewFromJSON(result, &flError);
    REQUIRE(flError == kFLNoError);

    const char* rev1Body =
            R"--({"_attachments":{"blob_/attached/0":{"content_type":"text/plain","digest":"sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=","length":27,"revpos":1,"stub":true},"blob_/attached/1":{"content_type":"text/plain","digest":"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=","length":10,"revpos":1,"stub":true},"blob_/attached/2":{"content_type":"text/plain","digest":"sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=","length":0,"revpos":1,"stub":true}},"_id":"att1","_rev":"1-b98a25d09a549dc2f68ac7b6a1acaf4da55e0f0d","attached":[{"@type":"blob","content_type":"text/plain","digest":"sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=","length":27},{"@type":"blob","content_type":"text/plain","digest":"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=","length":10},{"@type":"blob","content_type":"text/plain","digest":"sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=","length":0}]})--";

    // Notice that _attachments has 2 attachments in rev2Body, instead of 3 in rev1Body
    const char* rev2Body =
            R"--({"_attachments":{"blob_/attached/1":{"content_type":"text/plain","digest":"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=","length":10,"revpos":1,"stub":true},"blob_/attached/2":{"content_type":"text/plain","digest":"sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=","length":0,"revpos":1,"stub":true}},"_id":"att1","_rev":"1-b98a25d09a549dc2f68ac7b6a1acaf4da55e0f0d","attached":[{"@type":"blob","content_type":"text/plain","digest":"sha1-ERWD9RaGBqLSWOQ+96TZ6Kisjck=","length":27},{"@type":"blob","content_type":"text/plain","digest":"sha1-rATs731fnP+PJv2Pm/WXWZsCw48=","length":10},{"@type":"blob","content_type":"text/plain","digest":"sha1-2jmj7l5rSw0yVb/vlWAYkK/YBwk=","length":0}]})--";

    REQUIRE(rev1.toJSONString() == rev1Body);

    Dict _attachments = rev1.get("_attachments"_sl).asDict();
    REQUIRE(_attachments);
    MutableDict attachmentsMinus1 = _attachments.asMutable();
    auto        key0              = attachmentsMinus1.begin().key();
    attachmentsMinus1.remove(key0.asString());
    // rev1 is changed by attachmentsMinus1
    REQUIRE(rev1.toJSONString() == rev2Body);

    // Remove "blob_/attached/0" in SGW
    alloc_slice res = _sg.sendRemoteRequest("PUT", "/scratch/att1", rev1.toJSON(), false, HTTPStatus::Created);

    bool docDeleted = false;
    SECTION("Remove attachment in SGW before rev1 is synced") {
        C4Log("-------- Deleting and re-creating database --------");
        // Simulate the case where attachment is deleted in SGW before the rev is synced.
        // Since the rev on which SGW modifed is not in local, we will not receive
        // delta rev.
        deleteAndRecreateDB();
        docDeleted = true;
    }

    SECTION("Remove attachment in SGW after rev1 is synced") {
        // Simulate the case where attachment is deleted in SGW after the rev is synced.
        // We will receive delta rev is Delta Sync is enabled.
    }

    // The following Pull should fail because the first attachment is deleted in the remote.
    _expectedDocPullErrors = {"att1"};
    replicate(kC4Disabled, kC4OneShot);

    doc = c4doc_get(db, "att1"_sl, true, ERROR_INFO(error));
    if ( docDeleted ) {
        CHECK(!doc);
    } else {
        CHECK(c4rev_getGeneration(doc->revID) == 1);
    }
}

TEST_CASE_METHOD(ReplicatorSGTest, "KeepBody of Latest Synced Rev", "[.SyncServer]") {
    std::string revID;
    slice       docID = "Doc"_sl;
    {
        TransactionHelper t(db);
        revID = createNewRev(db, docID, nullslice, kFleeceBody);
    }

    c4::ref<C4Document> doc = c4db_getDoc(db, docID, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    REQUIRE(c4rev_getGeneration(doc->revID) == 1);
    CHECK(doc->selectedRev.flags == kRevLeaf);

    // Push rev 1-
    replicate(kC4OneShot, kC4Disabled);
    doc = c4db_getDoc(db, docID, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    REQUIRE(std::string(doc->revID) == revID);
    CHECK(doc->selectedRev.flags == (kRevLeaf | kRevKeepBody));  // After push, we have kRevKeepBody

    {
        // Modify rev 1 at SG
        Dict        props = c4doc_getProperties(doc);
        JSONEncoder enc;
        enc.beginDict();
        enc.writeKey("_id"_sl);
        enc.writeString(docID);
        enc.writeKey("_rev"_sl);
        enc.writeString(revID);
        for ( Dict::iterator i(props); i; ++i ) {
            enc.writeKey(i.keyString());
            auto value = i.value();
            enc.writeValue(value);
        }
        enc.writeKey("newKey"_sl);
        enc.writeString("newValue"_sl);
        enc.endDict();

        FLError     flError;
        alloc_slice res    = _sg.sendRemoteRequest("PUT", docID.asString(), enc.finish(), false, HTTPStatus::OK, false);
        fleece::Doc resDoc = Doc::fromJSON(res, &flError);
        REQUIRE(flError == kFLNoError);
        Dict resDict = resDoc.root().asDict();
        REQUIRE(resDict);
        REQUIRE((resDict.get("ok").asBool()));
        REQUIRE(c4rev_getGeneration(resDict.get("rev").asString()) == 2);
    }

    // Pull to get rev2
    replicate(kC4Disabled, kC4OneShot);
    doc = c4db_getDoc(db, docID, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    REQUIRE(c4rev_getGeneration(doc->revID) == 2);
    CHECK(doc->selectedRev.flags == (kRevLeaf | kRevKeepBody));
    REQUIRE(c4doc_selectParentRevision(doc));
    REQUIRE(c4rev_getGeneration(doc->selectedRev.revID) == 1);
    CHECK(doc->selectedRev.flags == 0);  // rev-1's KeepBody is cleared.

    replicate(kC4OneShot, kC4Disabled);
    doc = c4db_getDoc(db, docID, true, kDocGetAll, nullptr);
    REQUIRE(doc);
    REQUIRE(c4rev_getGeneration(doc->revID) == 2);
    CHECK(doc->selectedRev.flags == (kRevLeaf | kRevKeepBody));
    REQUIRE(c4doc_selectParentRevision(doc));
    REQUIRE(c4rev_getGeneration(doc->selectedRev.revID) == 1);
    CHECK(doc->selectedRev.flags == 0);  // rev-1's KeepBody is cleared.
}
