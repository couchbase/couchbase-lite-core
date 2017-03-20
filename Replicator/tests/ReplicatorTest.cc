//
//  ReplicatorTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/7/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#if DEBUG // This uses internal APIs that aren't exported in release builds of the replicator

#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4.hh"
#include "c4Socket+Internal.hh"
#include <iostream>
#include "c4Test.hh"
#include "Replicator.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


extern "C" void C4RegisterSocketFactory();


static const duration kCheckpointSaveDelay  = chrono::seconds(5);


class ReplicatorTest : public C4Test, Replicator::Delegate {
public:
    ReplicatorTest()
    :C4Test(0)
    ,address("ws", "localhost", 1235, "/scratch/_blipsync")
    {
        C4RegisterSocketFactory();
    }

    void runReplicator(Replicator::Options opts) {
        opts.checkpointSaveDelay = kCheckpointSaveDelay;
        replicator = new Replicator(db, DefaultProvider(), address, *this, opts);

        Log("Waiting for replication to complete...");
        while (replicator->activityLevel() > kC4Stopped)
            this_thread::sleep_for(chrono::milliseconds(100));
        Log(">>> Replication complete <<<");
    }

    virtual void replicatorActivityChanged(Replicator* repl, Replicator::ActivityLevel level) override {
        Log(">> Replicator is %s", ReplActor::kActivityLevelName[level]);
    }

    virtual void replicatorConnectionClosed(Replicator* repl, const CloseStatus &status) override {
        Log(">> Replicator closed with code=%d/%d, message=%.*s",
            status.reason, status.code, SPLAT(status.message));
        REQUIRE(status.reason == kWebSocketClose);
        REQUIRE(status.code == 1000);
    }

    Address address;
    Retained<Replicator> replicator;
};


TEST_CASE_METHOD(ReplicatorTest, "Real Push Empty DB", "[Push][.special]") {
    runReplicator(Replicator::Options::pushing());
}


TEST_CASE_METHOD(ReplicatorTest, "Real Push Non-Empty DB", "[Push][.special]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicator(Replicator::Options::pushing());
}


TEST_CASE_METHOD(ReplicatorTest, "Real Push Big DB", "[Push][.special]") {
    importJSONFile(sFixturesDir + "iTunesMusicLibrary.json");
    runReplicator(Replicator::Options::pushing());
}


TEST_CASE_METHOD(ReplicatorTest, "Real Pull DB", "[Pull][.special]") {
    runReplicator(Replicator::Options::pulling());
}

#endif
