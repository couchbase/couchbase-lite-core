//
// Actor.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
//
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Actor.hh"
#include "Logging.hh"
#include <mutex>


namespace litecore { namespace actor {

    void Actor::caughtException(const std::exception &x) {
        Warn("Caught exception in Actor %s: %s", actorName().c_str(), x.what());
    }


    void Actor::waitTillCaughtUp() {
        std::mutex mut;
        std::condition_variable cond;
        bool finished = false;
        enqueue(&Actor::_waitTillCaughtUp, &mut, &cond, &finished);
        
        std::unique_lock<std::mutex> lock(mut);
        cond.wait(lock, [&]{return finished;});
    }

    void Actor::_waitTillCaughtUp(std::mutex *mut, std::condition_variable *cond, bool *finished) {
        {
            std::lock_guard<std::mutex> lock(*mut);
            *finished = true;
        }
        cond->notify_one();
    }


} }
