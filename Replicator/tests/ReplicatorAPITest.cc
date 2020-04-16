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
#include "c4Socket.h"
#include "fleece/Fleece.hh"

using namespace fleece;

constexpr const C4Address ReplicatorAPITest::kDefaultAddress;
constexpr const C4String ReplicatorAPITest::kScratchDBName, ReplicatorAPITest::kITunesDBName,
                         ReplicatorAPITest::kWikipedia1kDBName,
                         ReplicatorAPITest::kProtectedDBName,
                         ReplicatorAPITest::kImagesDBName;

alloc_slice ReplicatorAPITest::sPinnedCert;


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

    REQUIRE(c4address_fromURL("http://192.168.7.20:59849/"_sl, &address, NULL));
    CHECK(address.scheme == "http"_sl);
    CHECK(address.hostname == "192.168.7.20"_sl);
    CHECK(address.port == 59849);
    CHECK(address.path == "/"_sl);

    REQUIRE(c4address_fromURL("http://[fe80:2f::3c]:59849/"_sl, &address, NULL));
    CHECK(address.scheme == "http"_sl);
    CHECK(address.hostname == "fe80:2f::3c"_sl);
    CHECK(address.port == 59849);
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


TEST_CASE_METHOD(ReplicatorAPITest, "API Create C4Replicator without start", "[Push]") {
    // For CBL-524 "Lazy c4replicator initialize cause memory leak"
    C4Error err;
    C4ReplicatorParameters params = {};
    params.push = kC4OneShot;
    params.pull = kC4Disabled;
    params.callbackContext = this;
    params.socketFactory = _socketFactory;

    _repl = c4repl_new(db, _address, kC4SliceNull, params, &err);
    C4Log("---- Releasing C4Replicator ----");
    _repl = nullptr;
}


// Test invalid URL scheme:
TEST_CASE_METHOD(ReplicatorAPITest, "API Invalid Scheme", "[C][Push][!throws]") {
    ExpectingExceptions x;
    _address.scheme = "http"_sl;
    C4Error err;
    CHECK(!c4repl_isValidRemote(_address, _remoteDBName, nullptr));
    REQUIRE(!startReplicator(kC4Disabled, kC4OneShot, &err));
    CHECK(err.domain == NetworkDomain);
    CHECK(err.code == kC4NetErrInvalidURL);
}


// Test missing or invalid database name:
TEST_CASE_METHOD(ReplicatorAPITest, "API Invalid URLs", "[C][Push][!throws]") {
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
TEST_CASE_METHOD(ReplicatorAPITest, "API Connection Failure", "[C][Push]") {
    ExpectingExceptions x;
    _address.hostname = C4STR("localhost");
    _address.port = 1;  // wrong port!
    _mayGoOffline = true;
    replicate(kC4Disabled, kC4OneShot, false);
    CHECK(_callbackStatus.error.domain == POSIXDomain);
    CHECK(_callbackStatus.error.code == ECONNREFUSED);
    CHECK(_callbackStatus.progress.unitsCompleted == 0);
    CHECK(_callbackStatus.progress.unitsTotal == 0);
    CHECK(_wentOffline);
    CHECK(_numCallbacksWithLevel[kC4Busy] == 0);
    CHECK(_numCallbacksWithLevel[kC4Idle] == 0);
}


// Test host-not-found error by connecting to a nonexistent hostname
TEST_CASE_METHOD(ReplicatorAPITest, "API DNS Lookup Failure", "[C][Push]") {
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


#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push", "[C][Push]") {
    importJSONLines(sFixturesDir + "names_100.json");

    createDB2();
    _enableDocProgressNotifications = true;
    replicate(kC4OneShot, kC4Disabled);

    CHECK(_docsEnded == 100);
    REQUIRE(c4db_getDocumentCount(db2) == 100);
}
#endif


#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push & Pull Deletion", "[C][Push][Pull]") {
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
#endif


TEST_CASE_METHOD(ReplicatorAPITest, "API Custom SocketFactory", "[C][Push][Pull]") {
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


#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "API Filtered Push", "[C][Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    createDB2();

    _pushFilter = [](C4String docID, C4String revID, C4RevisionFlags flags, FLDict flbody, void *context) {
        ((ReplicatorAPITest*)context)->_counter++;
        assert(docID.size > 0);
        assert(revID.size > 0);
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
#endif

// CBL-221
#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Stop with doc ended callback", "[C][Pull]") {
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
#endif

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Pending Document IDs", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    createDB2();

    FLSliceResult options {};

    C4Error err;
    C4ReplicatorParameters params = {};
    params.push = kC4OneShot;
    params.pull = kC4Disabled;
    params.callbackContext = this;
    params.socketFactory = _socketFactory;

    int expectedPending;
    SECTION("Normal") {
        expectedPending = 100;
    }

    SECTION("Filtered") {
        expectedPending = 99;
        params.pushFilter = [](C4String docID, C4String revID, C4RevisionFlags flags, FLDict flbody, void *context) {
            return FLSlice_Compare(docID, "0000005"_sl) != 0;
        };
    }

    SECTION("Set Doc IDs") {
        expectedPending = 2;
        FLEncoder e = FLEncoder_New();
        FLEncoder_BeginDict(e, 1);
        FLEncoder_WriteKey(e, FLSTR(kC4ReplicatorOptionDocIDs));
        FLEncoder_BeginArray(e, 2);
        FLEncoder_WriteString(e, FLSTR("0000002"));
        FLEncoder_WriteString(e, FLSTR("0000004"));
        FLEncoder_EndArray(e);
        FLEncoder_EndDict(e);
        options = FLEncoder_Finish(e, nullptr);
        params.optionsDictFleece = C4Slice(options);
        FLEncoder_Free(e);
    }

    _repl = c4repl_newLocal(db, (C4Database*)db2, params, &err);

    FLSliceResult_Release(options);

    C4SliceResult encodedDocIDs = c4repl_getPendingDocIDs(_repl, &err);
    REQUIRE(encodedDocIDs != nullslice);
    FLArray docIDs = FLValue_AsArray(FLValue_FromData(C4Slice(encodedDocIDs), kFLTrusted));
    CHECK(FLArray_Count(docIDs) == expectedPending);
    c4slice_free(encodedDocIDs);

    c4repl_start(_repl);
    while (c4repl_getStatus(_repl).level != kC4Stopped)
           this_thread::sleep_for(chrono::milliseconds(100));

    encodedDocIDs = c4repl_getPendingDocIDs(_repl, &err);
    CHECK(encodedDocIDs == nullslice);
}
#endif

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Is Document Pending", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    createDB2();

    FLSliceResult options {};

    C4Error err;
    C4ReplicatorParameters params = {};
    params.push = kC4OneShot;
    params.pull = kC4Disabled;
    params.callbackContext = this;
    params.socketFactory = _socketFactory;

    bool expectedIsPending = true;
    SECTION("Normal") {
        expectedIsPending = true;
    }

    SECTION("Filtered") {
        expectedIsPending = false;
        params.pushFilter = [](C4String docID, C4String revID, C4RevisionFlags flags, FLDict flbody, void *context) {
            return FLSlice_Compare(docID, "0000005"_sl) != 0;
        };
    }

    SECTION("Set Doc IDs") {
        expectedIsPending = false;
        FLEncoder e = FLEncoder_New();
        FLEncoder_BeginDict(e, 1);
        FLEncoder_WriteKey(e, FLSTR(kC4ReplicatorOptionDocIDs));
        FLEncoder_BeginArray(e, 2);
        FLEncoder_WriteString(e, FLSTR("0000002"));
        FLEncoder_WriteString(e, FLSTR("0000004"));
        FLEncoder_EndArray(e);
        FLEncoder_EndDict(e);
        options = FLEncoder_Finish(e, nullptr);
        params.optionsDictFleece = C4Slice(options);
        FLEncoder_Free(e);
    }

    _repl = c4repl_newLocal(db, (C4Database*)db2, params, &err);

    bool isPending = c4repl_isDocumentPending(_repl, "0000005"_sl, &err);
    CHECK(isPending == expectedIsPending);
    CHECK(err.code == 0);

    c4repl_start(_repl);
    while (c4repl_getStatus(_repl).level != kC4Stopped)
           this_thread::sleep_for(chrono::milliseconds(100));

    isPending = c4repl_isDocumentPending(_repl, "0000005"_sl, &err);
    CHECK(!isPending);
    CHECK(err.code == 0);
}
#endif

#ifdef COUCHBASE_ENTERPRISE
TEST_CASE_METHOD(ReplicatorAPITest, "Rapid Restarts", "[C][Push][Pull]") {
    importJSONLines(sFixturesDir + "names_100.json");

    createDB2();
    _mayGoOffline = true;
    C4Error err;
    REQUIRE(startReplicator(kC4Continuous, kC4Continuous, &err));
    waitForStatus(kC4Busy, 50);
    
    C4ReplicatorActivityLevel expected = kC4Stopped;
    SECTION("Stop / Start") {
        c4repl_stop(_repl);
        c4repl_start(_repl);
        expected = kC4Idle;
    }

    SECTION("Stop / Start / Stop") {
        c4repl_stop(_repl);
        c4repl_start(_repl);
        c4repl_stop(_repl);
    }

    SECTION("Suspend / Unsuspend") {
        c4repl_setSuspended(_repl, true);
        c4repl_setSuspended(_repl, false);
        expected = kC4Idle;
    }
    
    SECTION("Suspend / Unsuspend / Suspend") {
        c4repl_setSuspended(_repl, true);
        c4repl_setSuspended(_repl, false);
        c4repl_setSuspended(_repl, true);
        expected = kC4Offline;
    }
    
    SECTION("Stop / Suspend") {
        c4repl_stop(_repl);
        c4repl_setSuspended(_repl, true);
    }
    
    SECTION("Suspend / Stop") {
        c4repl_setSuspended(_repl, true);
        c4repl_stop(_repl);
    }
    
    SECTION("Stop / Unsuspend") {
        c4repl_stop(_repl);
        c4repl_setSuspended(_repl, false);
    }
    
    SECTION("Suspend / Stop / Unsuspend") {
        c4repl_setSuspended(_repl, true);
        c4repl_stop(_repl);
        c4repl_setSuspended(_repl, false);
    }
    
    SECTION("Stop / Stop") {
        c4repl_stop(_repl);
        c4repl_stop(_repl);
    }
    
    SECTION("Offline stop") {
        c4repl_setSuspended(_repl, true);
        waitForStatus(kC4Offline);
        c4repl_stop(_repl);
    }
    
    waitForStatus(expected);
    if(expected != kC4Stopped) {
        c4repl_stop(_repl);
        waitForStatus(kC4Stopped);
    }
}
#endif
