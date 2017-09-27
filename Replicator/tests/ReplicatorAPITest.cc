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


constexpr const C4Address ReplicatorAPITest::kDefaultAddress;
constexpr const C4String ReplicatorAPITest::kScratchDBName, ReplicatorAPITest::kITunesDBName,
                         ReplicatorAPITest::kWikipedia1kDBName,
                         ReplicatorAPITest::kProtectedDBName;


TEST_CASE("URL Parsing") {
    C4Address address;
    C4String dbName;

    REQUIRE(c4repl_parseURL("blip://localhost/dbname"_sl, &address, &dbName));
    CHECK(address.scheme == "blip"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4repl_parseURL("blips://localhost/dbname"_sl, &address, &dbName));
    CHECK(address.scheme == "blips"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4repl_parseURL("blips://localhost/dbname/"_sl, &address, &dbName));
    CHECK(address.scheme == "blips"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    ExpectingExceptions x;
    REQUIRE(!c4repl_parseURL(""_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip:"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip:/"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip://"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("http://localhost/dbname"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("://localhost/dbname"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("/dev/null"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("/dev/nu:ll"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip://localhost:-1/dbname"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip://localhost:666666/dbname"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip://localhost:x/dbname"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip://localhost:/foo"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip://localhost"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip://localhost/"_sl, &address, &dbName));
    REQUIRE(!c4repl_parseURL("blip://localhost/B@dn@m*"_sl, &address, &dbName));
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


#pragma mark - REAL-REPLICATOR (SYNC GATEWAY) TESTS


// The tests below are tagged [.RealReplicator] to keep them from running during normal testing.
// Instead, they have to be invoked manually via Catch command-line options.
// This is because they require that an external replication server is running.

TEST_CASE_METHOD(ReplicatorAPITest, "API Auth Failure", "[Push][.RealReplicator]") {
    _remoteDBName = kProtectedDBName;
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(_callbackStatus.error.domain == WebSocketDomain);
    CHECK(_callbackStatus.error.code == 401);
    CHECK(_headers["Www-Authenticate"].asString() == "Basic realm=\"Couchbase Sync Gateway\""_sl);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API ExtraHeaders", "[Push][.RealReplicator]") {
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


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Empty DB", "[Push][.RealReplicator]") {
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Non-Empty DB", "[Push][.RealReplicator]") {
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Empty Doc", "[Push][.RealReplicator]") {
    Encoder enc;
    enc.beginDict();
    enc.endDict();
    alloc_slice body = enc.finish();
    createRev("doc"_sl, kRevID, body);

    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Big DB", "[Push][.RealReplicator]") {
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json");
    replicate(kC4OneShot, kC4Disabled);
}


#if 0
TEST_CASE_METHOD(ReplicatorAPITest, "API Push Large-Docs DB", "[Push][.RealReplicator]") {
    importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json");
    replicate(kC4OneShot, kC4Disabled);
}
#endif


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull", "[Pull][.RealReplicator]") {
    _remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Continuous Push", "[Push][.RealReplicator]") {
    importJSONLines(sFixturesDir + "names_100.json");
    _stopWhenIdle = true;
    replicate(kC4Continuous, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Continuous Pull", "[Pull][.RealReplicator]") {
    _remoteDBName = kITunesDBName;
    _stopWhenIdle = true;
    replicate(kC4Disabled, kC4Continuous);
}


TEST_CASE_METHOD(ReplicatorAPITest, "Push & Pull Deletion", "[Push][Pull][.RealReplicator]") {
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


TEST_CASE_METHOD(ReplicatorAPITest, "Push & Pull Attachments", "[Push][Pull][ReplBlobs][.RealReplicator]") {
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


TEST_CASE_METHOD(ReplicatorAPITest, "Prove Attachments", "[Push][ReplBlobs][.RealReplicator]") {
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
