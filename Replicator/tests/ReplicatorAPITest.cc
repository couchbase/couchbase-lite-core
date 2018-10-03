//
//  ReplicatorAPITest.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/10/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ReplicatorAPITest.hh"
#include "c4Document+Fleece.h"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"

using namespace fleece;

constexpr const C4Address ReplicatorAPITest::kDefaultAddress;
constexpr const C4String ReplicatorAPITest::kScratchDBName, ReplicatorAPITest::kITunesDBName,
                         ReplicatorAPITest::kWikipedia1kDBName,
                         ReplicatorAPITest::kProtectedDBName,
                         ReplicatorAPITest::kImagesDBName;


TEST_CASE("URL Parsing") {
    C4Address address;
    C4String dbName;

    REQUIRE(c4address_fromURL("blip://localhost/dbname"_sl, &address, &dbName));
    CHECK(address.scheme == "blip"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("blip://localhost/dbname"_sl, &address, NULL));
    CHECK(address.scheme == "blip"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/dbname"_sl);

    REQUIRE(c4address_fromURL("blip://localhost/"_sl, &address, NULL));
    CHECK(address.scheme == "blip"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/"_sl);

    REQUIRE(c4address_fromURL("blips://localhost/dbname"_sl, &address, &dbName));
    CHECK(address.scheme == "blips"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("blips://localhost/dbname/"_sl, &address, &dbName));
    CHECK(address.scheme == "blips"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("blips://localhost/path/to/dbname"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("blips://localhost/path/to/dbname/"_sl, &address, &dbName));
    CHECK(address.scheme == "blips"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/path/to/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("file:///path/to/dbname/"_sl, &address, nullptr));
    CHECK(address.scheme == "file"_sl);
    CHECK(address.hostname == ""_sl);
    CHECK(address.port == 0);
    CHECK(address.path == "/path/to/dbname/"_sl);

    REQUIRE(c4address_fromURL("blips://localhost/path/to/dbname/"_sl, &address, NULL));
    CHECK(address.scheme == "blips"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/path/to/dbname/"_sl);

    REQUIRE(c4address_fromURL("blips://localhost/d"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("blips://localhost/p/d/"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("blips://localhost//p//d/"_sl, &address, &dbName));

    REQUIRE(!c4address_fromURL("blip://example.com/db@name"_sl, &address, &dbName));
    CHECK(dbName == "db@name"_sl);

    // The following URLs should all be rejected:
    ExpectingExceptions x;
    CHECK(!c4address_fromURL(""_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip:"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip:/"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("*://localhost/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("://localhost/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("/dev/null"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("/dev/nu:ll"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://localhost:-1/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://localhost:666666/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://localhost:x/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://localhost:/foo"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://localhost"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://localhost/"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://localhost/B^dn^m*"_sl, &address, &dbName));

    CHECK(!c4address_fromURL("blip://snej@example.com/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://snej@example.com:8080/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://snej:password@example.com/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("blip://snej:password@example.com:8080/db"_sl, &address, &dbName));
}


TEST_CASE("URL Generation") {
    CHECK(c4address_toURL({"ws"_sl, "foo.com"_sl, 8888, "/bar"_sl}) == "ws://foo.com:8888/bar"_sl);
    CHECK(c4address_toURL({"ws"_sl, "foo.com"_sl, 0,    "/"_sl})    == "ws://foo.com/"_sl);
}


// Test connection-refused error by connecting to a bogus port of localhost
TEST_CASE_METHOD(ReplicatorAPITest, "API Connection Failure", "[Push]") {
    _address.hostname = C4STR("localhost");
    _address.port = 1;  // wrong port!
    replicate(kC4Disabled, kC4OneShot, false);
    CHECK(_callbackStatus.error.domain == POSIXDomain);
    CHECK(_callbackStatus.error.code == ECONNREFUSED);
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);
}


// Test host-not-found error by connecting to a nonexistent hostname
TEST_CASE_METHOD(ReplicatorAPITest, "API DNS Lookup Failure", "[Push]") {
    _address.hostname = C4STR("qux.ftaghn.miskatonic.edu");
    replicate(kC4Disabled, kC4OneShot, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrUnknownHost);
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");

    createDB2();
    replicate(kC4OneShot, kC4Disabled);

    REQUIRE(c4db_getDocumentCount(db2) == 100);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push & Pull Deletion", "[Push][Pull]") {
    createRev("doc"_sl, kRevID, kFleeceBody);
    createRev("doc"_sl, kRev2ID, kEmptyFleeceBody, kRevDeleted);

    createDB2();
    replicate(kC4OneShot, kC4Disabled);

    c4::ref<C4Document> doc = c4doc_get(db2, "doc"_sl, true, nullptr);
    REQUIRE(doc);

    CHECK(doc->revID == kRev2ID);
    CHECK((doc->flags & kDocDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == kRevID);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Custom SocketFactory", "[Push][Pull]") {
    _address.hostname = C4STR("localhost");
    bool factoryCalled = false;
    C4SocketFactory factory = {};
    factory.context = &factoryCalled;
    factory.open = [](C4Socket* socket C4NONNULL, const C4Address* addr C4NONNULL,
                      C4Slice options, void *context) {
        *(bool*)context = true;
        c4socket_closed(socket, {NetworkDomain, kC4NetErrTooManyRedirects});
    };
    _socketFactory = &factory;
    replicate(kC4Disabled, kC4OneShot, false);
    REQUIRE(factoryCalled);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTooManyRedirects);
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);
}


#pragma mark - REAL-REPLICATOR (SYNC GATEWAY) TESTS


// The tests below are tagged [.SyncServer] to keep them from running during normal testing.
// Instead, they have to be invoked manually via Catch command-line options.
// This is because they require that an external replication server is running.
// The default URL the tests connect to is blip://localhost:4984/scratch/, but this can be
// overridden by setting environment vars REMOTE_HOST, REMOTE_PORT, REMOTE_DB.
// ** The tests will erase this database (via the SG REST API.) **

TEST_CASE_METHOD(ReplicatorAPITest, "API Auth Failure", "[.SyncServer]") {
    _remoteDBName = kProtectedDBName;
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(_callbackStatus.error.domain == WebSocketDomain);
    CHECK(_callbackStatus.error.code == 401);
    CHECK(_headers["Www-Authenticate"].asString() == "Basic realm=\"Couchbase Sync Gateway\""_sl);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API ExtraHeaders", "[.SyncServer]") {
    _remoteDBName = kProtectedDBName;

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


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Empty DB", "[.SyncServer]") {
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Non-Empty DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Empty Doc", "[.SyncServer]") {
    Encoder enc;
    enc.beginDict();
    enc.endDict();
    alloc_slice body = enc.finish();
    createRev("doc"_sl, kRevID, body);

    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Big DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    replicate(kC4OneShot, kC4Disabled);
}


#if 0
TEST_CASE_METHOD(ReplicatorAPITest, "API Push Large-Docs DB", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json");
    replicate(kC4OneShot, kC4Disabled);
}
#endif


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull", "[.SyncServer]") {
    _remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Continuous Push", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _stopWhenIdle = true;
    replicate(kC4Continuous, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Continuous Pull", "[.SyncServer]") {
    _remoteDBName = kITunesDBName;
    _stopWhenIdle = true;
    replicate(kC4Disabled, kC4Continuous);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Push & Pull Deletion", "[.SyncServer]") {
    createRev("doc"_sl, kRevID, kFleeceBody);
    createRev("doc"_sl, kRev2ID, kEmptyFleeceBody, kRevDeleted);

    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();
    createRev("doc"_sl, kRevID, kFleeceBody);

    replicate(kC4Disabled, kC4OneShot);

    c4::ref<C4Document> doc = c4doc_get(db, "doc"_sl, true, nullptr);
    REQUIRE(doc);

    CHECK(doc->revID == kRev2ID);
    CHECK((doc->flags & kDocDeleted) != 0);
    CHECK((doc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == kRevID);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Push & Pull Attachments", "[.SyncServer]") {
    vector<string> attachments = {"Hey, this is an attachment!", "So is this", ""};
    vector<C4BlobKey> blobKeys;
    {
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments("att1"_sl, attachments, "text/plain");
    }

    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db, "att1"_sl, true, &error);
    REQUIRE(doc);
    alloc_slice before = c4doc_bodyAsJSON(doc, true, &error);
    doc = nullptr;
    C4Log("Original doc: %.*s", SPLAT(before));

    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();

    replicate(kC4Disabled, kC4OneShot);

    doc = c4doc_get(db, "att1"_sl, true, &error);
    REQUIRE(doc);
    alloc_slice after = c4doc_bodyAsJSON(doc, true, &error);
    C4Log("Pulled doc: %.*s", SPLAT(after));

    // Is the pulled identical to the original?
    CHECK(after == before);

    // Did we get all of its attachments?
    auto blobStore = c4db_getBlobStore(db, &error);
    for( auto key : blobKeys) {
        alloc_slice blob = c4blob_getContents(blobStore, key, &error);
        CHECK(blob);
    }
}


TEST_CASE_METHOD(ReplicatorAPITest, "Prove Attachments", "[.SyncServer]") {
    vector<string> attachments = {"Hey, this is an attachment!"};
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


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull Big Attachments", "[.SyncServer]") {
    _remoteDBName = kImagesDBName;
    replicate(kC4Disabled, kC4OneShot);

    C4Error error;
    c4::ref<C4Document> doc = c4doc_get(db, "Abstract"_sl, true, &error);
    REQUIRE(doc);
    auto root = Value::fromData(doc->selectedRev.body).asDict();
    auto attach = root.get("_attachments"_sl).asDict().get("Abstract.jpg"_sl).asDict();
    REQUIRE(attach);
    CHECK(attach.get("content_type").asString() == "image/jpeg"_sl);
    slice digest = attach.get("digest").asString();
    CHECK(digest == "sha1-9g3HeOewh8//ctPcZkh03o+A+PQ="_sl);
    C4BlobKey blobKey;
    c4blob_keyFromString(digest, &blobKey);
    auto size = c4blob_getSize(c4db_getBlobStore(db, nullptr), blobKey);
    CHECK(size == 15198281);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Conflict", "[.SyncServer]") {
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);

    sendRemoteRequest("PUT", "0000013", "{\"_rev\":\"1-3cb9cfb09f3f0b5142e618553966ab73539b8888\","
                                          "\"serverSideUpdate\":true}"_sl);

    createRev("0000013"_sl, "2-f000"_sl, kFleeceBody);

    c4::ref<C4Document> doc = c4doc_get(db, C4STR("0000013"), true, nullptr);
    REQUIRE(doc);
	C4Slice revID = C4STR("2-f000");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = C4STR("1-3cb9cfb09f3f0b5142e618553966ab73539b8888");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);

    C4Log("-------- Pushing Again (conflict) --------");
    _expectedDocPushErrors = {"0000013"};
    replicate(kC4OneShot, kC4Disabled);

    C4Log("-------- Pulling --------");
    _expectedDocPushErrors = { };
    _expectedDocPullErrors = {"0000013"};
    replicate(kC4Disabled, kC4OneShot);

    C4Log("-------- Checking Conflict --------");
    doc = c4doc_get(db, C4STR("0000013"), true, nullptr);
    REQUIRE(doc);
    CHECK((doc->flags & kDocConflicted) != 0);
	revID = C4STR("2-f000");
    CHECK(doc->selectedRev.revID == revID);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = C4STR("1-3cb9cfb09f3f0b5142e618553966ab73539b8888");
    CHECK(doc->selectedRev.revID == revID);
#if 0 // FIX: These checks fail due to issue #402; re-enable when fixing that bug
    CHECK(doc->selectedRev.body.size > 0);
    CHECK((doc->selectedRev.flags & kRevKeepBody) != 0);
#endif
    REQUIRE(c4doc_selectCurrentRevision(doc));
    REQUIRE(c4doc_selectNextRevision(doc));
	revID = C4STR("2-883a2dacc15171a466f76b9d2c39669b");
    CHECK(doc->selectedRev.revID == revID);
    CHECK((doc->selectedRev.flags & kRevIsConflict) != 0);
    CHECK(doc->selectedRev.body.size > 0);
    REQUIRE(c4doc_selectParentRevision(doc));
	revID = C4STR("1-3cb9cfb09f3f0b5142e618553966ab73539b8888");
    CHECK(doc->selectedRev.revID == revID);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Update Once-Conflicted Doc", "[.SyncServer]") {
    // For issue #448.
    // Create a conflicted doc on SG, and resolve the conflict:
    _remoteDBName = "scratch_allows_conflicts"_sl;
    flushScratchDatabase();
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"_rev\":\"1-aaaa\",\"foo\":1}"_sl);
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"_revisions\":{\"start\":2,\"ids\":[\"bbbb\",\"aaaa\"]},\"foo\":2.1}"_sl);
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"_revisions\":{\"start\":2,\"ids\":[\"cccc\",\"aaaa\"]},\"foo\":2.2}"_sl);
    sendRemoteRequest("PUT", "doc?new_edits=false", "{\"_revisions\":{\"start\":3,\"ids\":[\"dddd\",\"cccc\"]},\"_deleted\":true}"_sl);

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(kC4OneShot, kC4OneShot);

    // Verify doc:
    c4::ref<C4Document> doc = c4doc_get(db, "doc"_sl, true, nullptr);
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
    auto body = sendRemoteRequest("GET", "doc");
	C4Slice bodySlice = C4STR("{\"_id\":\"doc\",\"_rev\":\"3-ffff\",\"answer\":42}");
    CHECK(C4Slice(body) == bodySlice);
}


