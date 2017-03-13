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
#include "Logging.hh"
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
    ReplicatorAPITest()
    :C4Test(0)
    {
    }

    void logState(C4ReplicatorState state) {
        char message[200];
        c4error_getMessageC(state.error, message, sizeof(message));
        Log(">>> C4Replicator state: level=%d, error=%d/%d: %s",
            state.level, state.error.domain, state.error.code, message);
    }

    void stateChanged(C4Replicator *r, C4ReplicatorState s) {
        REQUIRE(r == repl);
        callbackState = s;
        ++numCallbacks;
        numCallbacksWithLevel[(int)s.level]++;
        logState(callbackState);
    }

    static void onStateChanged(C4Replicator *replicator,
                               C4ReplicatorState state,
                               void *context)
    {
        ((ReplicatorAPITest*)context)->stateChanged(replicator, state);
    }
    
    
    void replicate(C4ReplicatorMode push, C4ReplicatorMode pull) {
        C4Error err;
        repl = c4repl_new(db, address, push, pull, onStateChanged, this, &err);
        REQUIRE(repl);
        C4ReplicatorState state = c4repl_getState(repl);
        logState(state);
        CHECK(state.level == kC4Connecting);
        CHECK(state.error.code == 0);

        while ((state = c4repl_getState(repl)).level != kC4Stopped)
            this_thread::sleep_for(chrono::milliseconds(100));

        CHECK(state.error.code == 0);
        CHECK(numCallbacks > 0);
        CHECK(numCallbacksWithLevel[kC4Busy] > 0);
        CHECK(numCallbacksWithLevel[kC4Stopped] > 0);
        CHECK(callbackState.level == state.level);
        CHECK(callbackState.error.domain == state.error.domain);
        CHECK(callbackState.error.code == state.error.code);
    }
    

    C4Address address {kC4Replicator2Scheme, C4STR("localhost"), 1235, C4STR("scratch")};
    C4Replicator *repl {nullptr};
    C4ReplicatorState callbackState {};
    int numCallbacks {0};
    int numCallbacksWithLevel[4] = {0};
};



TEST_CASE_METHOD(ReplicatorAPITest, "API Push Empty DB", "[Push][.special]") {
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Push Non-Empty DB", "[Push][.special]") {
    importJSONLines(sFixturesDir + "names_100.json");
    replicate(kC4OneShot, kC4Disabled);
}


TEST_CASE_METHOD(ReplicatorAPITest, "API Pull", "[Push][.special]") {
    replicate(kC4Disabled, kC4OneShot);
}


