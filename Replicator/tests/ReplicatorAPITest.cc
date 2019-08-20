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

Retained<litecore::crypto::Cert> ReplicatorAPITest::sPinnedCert;



TEST_CASE("URL Parsing") {
    C4Address address;
    C4String dbName;

    REQUIRE(c4address_fromURL("ws://localhost/dbname"_sl, &address, &dbName));
    CHECK(address.scheme == "ws"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("ws://localhost/dbname"_sl, &address, NULL));
    CHECK(address.scheme == "ws"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/dbname"_sl);

    REQUIRE(c4address_fromURL("ws://localhost/"_sl, &address, NULL));
    CHECK(address.scheme == "ws"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 80);
    CHECK(address.path == "/"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/dbname"_sl, &address, &dbName));
    CHECK(address.scheme == "wss"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/dbname/"_sl, &address, &dbName));
    CHECK(address.scheme == "wss"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/path/to/dbname"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("wss://localhost/path/to/dbname/"_sl, &address, &dbName));
    CHECK(address.scheme == "wss"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/path/to/"_sl);
    CHECK(dbName == "dbname"_sl);

    REQUIRE(c4address_fromURL("file:///path/to/dbname/"_sl, &address, nullptr));
    CHECK(address.scheme == "file"_sl);
    CHECK(address.hostname == ""_sl);
    CHECK(address.port == 0);
    CHECK(address.path == "/path/to/dbname/"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/path/to/dbname/"_sl, &address, NULL));
    CHECK(address.scheme == "wss"_sl);
    CHECK(address.hostname == "localhost"_sl);
    CHECK(address.port == 443);
    CHECK(address.path == "/path/to/dbname/"_sl);

    REQUIRE(c4address_fromURL("wss://localhost/d"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("wss://localhost/p/d/"_sl, &address, &dbName));
    REQUIRE(c4address_fromURL("wss://localhost//p//d/"_sl, &address, &dbName));

    REQUIRE(!c4address_fromURL("ws://example.com/db@name"_sl, &address, &dbName));
    CHECK(dbName == "db@name"_sl);

    // The following URLs should all be rejected:
    ExpectingExceptions x;
    CHECK(!c4address_fromURL(""_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws:"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws:/"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("*://localhost/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("://localhost/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("/dev/null"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("/dev/nu:ll"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost:-1/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost:666666/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost:x/dbname"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost:/foo"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost/"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://localhost/B^dn^m*"_sl, &address, &dbName));

    CHECK(!c4address_fromURL("ws://snej@example.com/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://snej@example.com:8080/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://snej:password@example.com/db"_sl, &address, &dbName));
    CHECK(!c4address_fromURL("ws://snej:password@example.com:8080/db"_sl, &address, &dbName));
}


TEST_CASE("URL Generation") {
    CHECK(c4address_toURL({"ws"_sl, "foo.com"_sl, 8888, "/bar"_sl}) == "ws://foo.com:8888/bar"_sl);
    CHECK(c4address_toURL({"ws"_sl, "foo.com"_sl, 0,    "/"_sl})    == "ws://foo.com/"_sl);
}


// Test invalid URL scheme:
TEST_CASE_METHOD(ReplicatorAPITest, "API Invalid Scheme", "[Push][!throws]") {
    ExpectingExceptions x;
    _address.scheme = "http"_sl;
    C4Error err;
    CHECK(!c4repl_isValidRemote(_address, _remoteDBName, nullptr));
    REQUIRE(!startReplicator(kC4Disabled, kC4OneShot, &err));
    CHECK(err.domain == NetworkDomain);
    CHECK(err.code == kC4NetErrInvalidURL);
}


// Test missing or invalid database name:
TEST_CASE_METHOD(ReplicatorAPITest, "API Invalid URLs", "[Push][!throws]") {
    ExpectingExceptions x;
    _remoteDBName = ""_sl;
    C4Error err;
    CHECK(!c4repl_isValidRemote(_address, _remoteDBName, nullptr));
    REQUIRE(!startReplicator(kC4Disabled, kC4OneShot, &err));
    CHECK(err.domain == NetworkDomain);
    CHECK(err.code == kC4NetErrInvalidURL);

    _remoteDBName = "Invalid Name"_sl;
    err = {};
    CHECK(!c4repl_isValidRemote(_address, _remoteDBName, nullptr));
    REQUIRE(!startReplicator(kC4Disabled, kC4OneShot, &err));
    CHECK(err.domain == NetworkDomain);
    CHECK(err.code == kC4NetErrInvalidURL);
}


// Test connection-refused error by connecting to a bogus port of localhost
TEST_CASE_METHOD(ReplicatorAPITest, "API Connection Failure", "[Push]") {
    ExpectingExceptions x;
    _address.hostname = C4STR("localhost");
    _address.port = 1;  // wrong port!
    replicate(kC4Disabled, kC4OneShot, false);
    CHECK(_callbackStatus.error.domain == POSIXDomain);
    CHECK(_callbackStatus.error.code == ECONNREFUSED);
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);
    CHECK(_numCallbacksWithLevel[kC4Busy] == 0);
    CHECK(_numCallbacksWithLevel[kC4Idle] == 0);
}


// Test host-not-found error by connecting to a nonexistent hostname
TEST_CASE_METHOD(ReplicatorAPITest, "API DNS Lookup Failure", "[Push]") {
    ExpectingExceptions x;
    _address.hostname = C4STR("qux.ftaghn.miskatonic.edu");
    replicate(kC4Disabled, kC4OneShot, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrUnknownHost);
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);
    CHECK(_numCallbacksWithLevel[kC4Busy] == 0);
    CHECK(_numCallbacksWithLevel[kC4Idle] == 0);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");

    createDB2();
    _enableDocProgressNotifications = true;
    replicate(kC4OneShot, kC4Disabled);

    CHECK(_docsEnded == 100);
    REQUIRE(c4db_getDocumentCount(db2) == 100);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push & Pull Deletion", "[Push][Pull]") {
    createRev("doc"_sl, kRevID, kFleeceBody);
    createRev("doc"_sl, kRev2ID, kEmptyFleeceBody, kRevDeleted);

    createDB2();
    _enableDocProgressNotifications = true;
    replicate(kC4OneShot, kC4Disabled);
    CHECK(_docsEnded == 1);

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


TEST_CASE_METHOD(ReplicatorAPITest, "API Filtered Push", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    createDB2();

    _pushFilter = [](C4String docID, C4RevisionFlags flags, FLDict flbody, void *context) {
        ((ReplicatorAPITest*)context)->_counter++;
        assert(docID.size > 0);
        Dict body(flbody);
        assert(body.count() >= 4);
        return body["gender"_sl].asString() == "male"_sl;
    };

    _enableDocProgressNotifications = true;
    replicate(kC4OneShot, kC4Disabled);

    CHECK(_counter == 100);
    CHECK(_docsEnded == 45);
    CHECK(c4db_getDocumentCount(db2) == 45);
}

// CBL-221
TEST_CASE_METHOD(ReplicatorAPITest, "Stop with doc ended callback", "[Pull]") {
    createDB2();
    // Need a large enough data set so that the pulled documents come
    // through in more than one batch
    importJSONLines(sFixturesDir + "iTunesMusicLibrary.json", 15.0, false, db2);
    
    _enableDocProgressNotifications = true;
    
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        ((ReplicatorAPITest*)context)->_docsEnded += numDocs;
        c4repl_stop(repl);
    };
    
    replicate(kC4Disabled, kC4Continuous);
    
    // Not being equal implies that some of the doc ended callbacks failed
    // (presumably because of CBL-221 which causes an actor internal assertion
    // failure that cannot be detected from the outside)
    CHECK(c4db_getDocumentCount(db) == _docsEnded);
}
