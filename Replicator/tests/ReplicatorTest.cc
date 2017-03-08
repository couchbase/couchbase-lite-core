//
//  ReplicatorTest.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/7/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4.hh"
#include <iostream>
#include "c4Test.hh"
#include "Replicator.hh"
#include "LibWSProvider.hh"
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


static const duration kLatency              = chrono::milliseconds(100);
static const duration kCheckpointSaveDelay  = chrono::milliseconds(500);


class ReplicatorTest : public C4Test, Replicator::Delegate {
public:
    ReplicatorTest()
    :C4Test(0)
    ,provider()
    ,address("ws", "localhost", 1235, "scratch/_blipsync")
    {
        provider.startEventLoop();
    }

    void runReplicator(Replicator::Options opts) {
        opts.checkpointSaveDelay = kCheckpointSaveDelay;
        replicator = new Replicator(db, provider, address, *this, opts);

        Log("Waiting for replication to complete...");
        while (replicator->connection())
            this_thread::sleep_for(chrono::milliseconds(100));
        Log(">>> Replication complete <<<");
        checkpointID = replicator->checkpointID();
    }

    virtual void replicatorActivityChanged(Replicator* repl, Replicator::ActivityLevel level) override {
        const char *kLevelNames[] = {"finished!", "connecting", "idle", "busy"};
        Log(">> Replicator is %s", kLevelNames[level]);
    }

    virtual void replicatorCloseStatusChanged(Replicator* repl, const CloseStatus &status) override {
        Log(">> Replicator closed with code=%d/%d, message=%.*s",
            status.reason, status.code, SPLAT(status.message));
    }


    LibWSProvider provider;
    Address address;
    Retained<Replicator> replicator;
    alloc_slice checkpointID;
//    unique_ptr<thread> parallelThread;
//    future<void> parallelThreadDone;
};


TEST_CASE_METHOD(ReplicatorTest, "Real Push Empty DB", "[Push]") {
    runReplicator(Replicator::Options::pushing());
}


TEST_CASE_METHOD(ReplicatorTest, "Real Push Non-Empty DB", "[Push]") {
    importJSONLines(sFixturesDir + "names_100.json");
    runReplicator(Replicator::Options::pushing());
}
