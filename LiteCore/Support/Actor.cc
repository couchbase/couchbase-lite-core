//
// Actor.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Actor.hh"
#include "Logging.hh"
#include "Channel.hh"
#include <mutex>
#include <chrono>


namespace litecore { namespace actor {

    void Actor::caughtException(const std::exception &x) {
        Warn("Caught exception in Actor %s: %s", actorName().c_str(), x.what());
    }


    void Actor::waitTillCaughtUp() {
        using namespace std::chrono_literals;
        std::mutex mut;
        std::condition_variable cond;
        bool finished = false;
        enqueue(FUNCTION_TO_QUEUE(Actor::_waitTillCaughtUp), &mut, &cond, &finished);
        
        std::unique_lock<std::mutex> lock(mut);
        logInfo("Actor %s(%p) going to wait ...", actorName().c_str(), this);
//        cond.wait(lock, [&]{return finished;});
        while (!finished) {
            if (cond.wait_for(lock, 2s) == std::cv_status::timeout) {
                logInfo("Actor %s(%p) timed out waiting...\nTask Threads: %s", actorName().c_str(), this, TaskDbg::dumpTasks().c_str());
            }
        }
    }

    void Actor::_waitTillCaughtUp(std::mutex *mut, std::condition_variable *cond, bool *finished) {
        std::lock_guard<std::mutex> lock(*mut);
        *finished = true;
        // It's important to keep the mutex locked while calling notify_one. This ensures that
        // `waitTillCaughtUp` won't wake up and return, invalidating `*cond`, before I have a
        // chance to `notify_one` on it. (CBL-984)
        logInfo("Actor %s(%p) caught up.", actorName().c_str(), this);
        cond->notify_one();
    }


} }
