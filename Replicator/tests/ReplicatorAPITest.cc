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
#include "c4Replicator.h"
#include "StringUtil.hh"
#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

extern "C" void C4RegisterSocketFactory();

using namespace std;
using namespace fleece;
using namespace litecore;


class ReplicatorAPITest : public C4Test {
public:
    constexpr static const C4Address kDefaultAddress {kC4Replicator2Scheme, C4STR("localhost"), 4984};
    constexpr static const C4String kScratchDBName = C4STR("scratch");
    constexpr static const C4String kITunesDBName = C4STR("itunes");
    constexpr static const C4String kWikipedia1kDBName = C4STR("wikipedia1k");

    ReplicatorAPITest()
    :C4Test(0)
    {
        C4RegisterSocketFactory();
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
        C4Log(">>> C4Replicator state: level=%d, progress=%llu/%llu, error=%d/%d: %s",
              status.level, status.progress.completed, status.progress.total,
              status.error.domain, status.error.code, message);
    }

    void stateChanged(C4Replicator *r, C4ReplicatorStatus s) {
        REQUIRE(r == repl);
        callbackStatus = s;
        ++numCallbacks;
        numCallbacksWithLevel[(int)s.level]++;
        logState(callbackStatus);
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
                          push, pull, onStateChanged, this, &err);
        REQUIRE(repl);
        C4ReplicatorStatus status = c4repl_getStatus(repl);
        logState(status);
        CHECK(status.level == kC4Connecting);
        CHECK(status.error.code == 0);

        while ((status = c4repl_getStatus(repl)).level != kC4Stopped)
            this_thread::sleep_for(chrono::milliseconds(100));

        CHECK(numCallbacks > 0);
        if (expectSuccess) {
            CHECK(numCallbacksWithLevel[kC4Busy] > 0);
            CHECK(status.error.code == 0);
        }
        CHECK(numCallbacksWithLevel[kC4Stopped] > 0);
        CHECK(callbackStatus.level == status.level);
        CHECK(callbackStatus.error.domain == status.error.domain);
        CHECK(callbackStatus.error.code == status.error.code);
    }

    C4Database *db2 {nullptr};
    C4Address address {kDefaultAddress};
    C4String remoteDBName {kScratchDBName};
    C4Replicator *repl {nullptr};
    C4ReplicatorStatus callbackStatus {};
    int numCallbacks {0};
    int numCallbacksWithLevel[5] {0};
};


constexpr const C4Address ReplicatorAPITest::kDefaultAddress;
constexpr const C4String ReplicatorAPITest::kScratchDBName, ReplicatorAPITest::kITunesDBName,
                         ReplicatorAPITest::kWikipedia1kDBName;



// Try to connect to a nonexistent server (port 1 of localhost) and verify connection error.
TEST_CASE_METHOD(ReplicatorAPITest, "API Connection Failure", "[Push]") {
    address.hostname = C4STR("localhost");
    address.port = 1;
    replicate(kC4OneShot, kC4Disabled, false);
    CHECK(callbackStatus.progress.completed == 0);
    CHECK(callbackStatus.progress.total == 0);
    CHECK(callbackStatus.error.domain == POSIXDomain);
    CHECK(callbackStatus.error.code == ECONNREFUSED);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Empty DB", "[Push][.special]") {
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Non-Empty DB", "[Push][.special]") {
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Big DB", "[Push][.special]") {
    importJSONFile(sFixturesDir + "iTunesMusicLibrary.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Large-Docs DB", "[Push][.special]") {
    importJSONLines(sFixturesDir + "en-wikipedia-articles-1000-1.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull", "[Push][.special]") {
    replicate(kC4Disabled, kC4OneShot);
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
