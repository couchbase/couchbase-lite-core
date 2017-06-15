//
//  ReplicatorAPITest.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/10/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4.hh"
#include <iostream>
#include "c4Test.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

using namespace std;
using namespace fleece;
using namespace litecore;


class ReplicatorAPITest : public C4Test {
public:
    // Default address to replicate with (individual tests can override this):
    constexpr static const C4Address kDefaultAddress {kC4Replicator2Scheme,
                                                      C4STR("localhost"),
                                                      4984};
    // Common database names:
    constexpr static const C4String kScratchDBName = C4STR("scratch");
    constexpr static const C4String kITunesDBName = C4STR("itunes");
    constexpr static const C4String kWikipedia1kDBName = C4STR("wikipedia1k");
    constexpr static const C4String kProtectedDBName = C4STR("seekrit");

    ReplicatorAPITest()
    :C4Test(0)
    {
        // Environment variables can also override the default address above:
        const char *hostname = getenv("REMOTE_HOST");
        if (hostname)
            address.hostname = c4str(hostname);
        const char *portStr = getenv("REMOTE_PORT");
        if (portStr)
            address.port = (uint16_t)strtol(portStr, nullptr, 10);
        const char *remoteDB = getenv("REMOTE_DB");
        if (remoteDB)
            remoteDBName = c4str(remoteDB);
    }

    ~ReplicatorAPITest() {
        c4repl_free(repl);
        c4db_free(db2);
    }

    void logState(C4ReplicatorStatus status) {
        char message[200];
        c4error_getMessageC(status.error, message, sizeof(message));
        C4Log("*** C4Replicator state: %-s, progress=%llu/%llu, error=%d/%d: %s",
              kC4ReplicatorActivityLevelNames[status.level],
              status.progress.completed, status.progress.total,
              status.error.domain, status.error.code, message);
    }

    void stateChanged(C4Replicator *r, C4ReplicatorStatus s) {
        Assert(r == repl);      // can't call REQUIRE on a background thread
        callbackStatus = s;
        ++numCallbacks;
        numCallbacksWithLevel[(int)s.level]++;
        logState(callbackStatus);

        if (!_headers) {
            _headers = AllocedDict(alloc_slice(c4repl_getResponseHeaders(repl)));
            if (_headers) {
                for (Dict::iterator header(_headers); header; ++header)
                    C4Log("    %.*s: %.*s", SPLAT(header.keyString()), SPLAT(header.value().asString()));
            }
        }

        if (!db2) {  // i.e. this is a real WebSocket connection
            if (s.level > kC4Connecting
                    || (s.level == kC4Stopped && s.error.domain == WebSocketDomain))
                Assert(_headers);
        }
    }

    static void onStateChanged(C4Replicator *replicator,
                               C4ReplicatorStatus status,
                               void *context)
    {
        ((ReplicatorAPITest*)context)->stateChanged(replicator, status);
    }
    
    
    void replicate(C4ReplicatorMode push, C4ReplicatorMode pull, bool expectSuccess =true) {
        C4Error err;
        repl = c4repl_new(db, address, remoteDBName, db2,
                          push, pull, options.data(),
                          onStateChanged, this, &err);
        REQUIRE(repl);
        C4ReplicatorStatus status = c4repl_getStatus(repl);
        logState(status);
        CHECK(status.level == kC4Connecting);
        CHECK(status.error.code == 0);

        while ((status = c4repl_getStatus(repl)).level != kC4Stopped)
            this_thread::sleep_for(chrono::milliseconds(100));

        CHECK(numCallbacks > 0);
        if (expectSuccess) {
            CHECK(status.error.code == 0);
            CHECK(numCallbacksWithLevel[kC4Busy] > 0);
            //CHECK(_gotHeaders);   //FIX: Enable this when civetweb can return HTTP headers
        }
        CHECK(numCallbacksWithLevel[kC4Stopped] > 0);
        CHECK(callbackStatus.level == status.level);
        CHECK(callbackStatus.error.domain == status.error.domain);
        CHECK(callbackStatus.error.code == status.error.code);
    }

    C4Database *db2 {nullptr};
    C4Address address {kDefaultAddress};
    C4String remoteDBName {kScratchDBName};
    AllocedDict options;
    C4Replicator *repl {nullptr};
    C4ReplicatorStatus callbackStatus {};
    int numCallbacks {0};
    int numCallbacksWithLevel[5] {0};
    AllocedDict _headers;
};


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
    address.hostname = C4STR("localhost");
    address.port = 1;
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(callbackStatus.error.domain == POSIXDomain);
    CHECK(callbackStatus.error.code == ECONNREFUSED);
    CHECK(callbackStatus.progress.completed == 0);
    CHECK(callbackStatus.progress.total == 0);
}


// Test host-not-found error by connecting to a nonexistent hostname
TEST_CASE_METHOD(ReplicatorAPITest, "API DNS Lookup Failure", "[Push]") {
    address.hostname = C4STR("qux.ftaghn.miskatonic.edu");
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(callbackStatus.error.domain == NetworkDomain);
    CHECK(callbackStatus.error.code == kC4NetErrUnknownHost);
    CHECK(callbackStatus.progress.completed == 0);
    CHECK(callbackStatus.progress.total == 0);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Loopback Push", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");

    auto db2Path = TempDir() + "cbl_core_test2";
    auto db2PathSlice = c4str(db2Path.c_str());

    auto config = c4db_getConfig(db);
    C4Error error;
    if (!c4db_deleteAtPath(db2PathSlice, config, &error))
        REQUIRE(error.code == 0);
    db2 = c4db_open(db2PathSlice, config, &error);
    REQUIRE(db2 != nullptr);

    address = { };
    remoteDBName = nullslice;

    replicate(kC4OneShot, kC4Disabled);

    REQUIRE(c4db_getDocumentCount(db2) == 100);
}


// The tests below are tagged [.RealReplicator] to keep them from running during normal testing.
// Instead, they have to be invoked manually via Catch command-line options.
// This is because they require that an external replication server is running.

TEST_CASE_METHOD(ReplicatorAPITest, "API Auth Failure", "[Push][.RealReplicator]") {
    remoteDBName = kProtectedDBName;
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(callbackStatus.error.domain == WebSocketDomain);
    CHECK(callbackStatus.error.code == 401);
    CHECK(_headers["Www-Authenticate"].asString() == "Basic realm=\"Couchbase Sync Gateway\""_sl);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API ExtraHeaders", "[Push][.RealReplicator]") {
    remoteDBName = kProtectedDBName;

    // Use the extra-headers option to add HTTP Basic auth:
    Encoder enc;
    enc.beginDict();
    enc.writeKey(C4STR(kC4ReplicatorOptionExtraHeaders));
    enc.beginDict();
    enc.writeKey("Authorization"_sl);
    enc.writeString("Basic cHVwc2hhdzpmcmFuaw=="_sl);  // that's user 'pupshaw', password 'frank'
    enc.endDict();
    enc.endDict();
    options = AllocedDict(enc.finish());

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


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Large-Docs DB", "[Push][.RealReplicator]") {
    importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull", "[Pull][.RealReplicator]") {
    remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4OneShot);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Continuous Pull", "[Pull][.neverending]") {
    remoteDBName = kITunesDBName;
    replicate(kC4Disabled, kC4Continuous);
}
