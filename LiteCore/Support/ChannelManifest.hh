//
// ChannelManifest.hh
//
// Copyright (c) 2020 Couchbase, Inc All rights reserved.
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

#pragma once

#include "Channel.hh"

#if ACTORS_USE_MANIFESTS

#include <chrono>
#include <list>
#include <iostream>
#include <mutex>

#ifdef ACTORS_USE_GCD
#include <dispatch/dispatch.h>
#endif

namespace litecore::actor {
    class Actor;
    
    /** A simple class to keep track of nested mailbox calls, similar to the way that Apple tracks
     *  through GCD enqueue calls.  The way it works is as follows, and is common between both
     *  GCD and threaded mailbox:
     *
     *  1. On the initial call to enqueue, or enqueueAfter, a thread local manifest is checked for existence.
     *  =====
     *  2. The manifest does not exist, and it is created and captured by the block that will run inside the mailbox
     *  3. Inside the block, the thread local manifest is set to the captured manifest, so that any calls to enqueue
     *     or enqueueAfter that occur as the result of the block will notice the manifest in step 1
     *  =====
     *  2. The manifest exists, and is saved in a local variable and captured by the block that will run inside the mailbox
     *  3. Inside the block, the thread local manifest is set to the captured manifest (see reasons in alternate step 3 above)
     *  =====
     *  4. After the block is finished, the thread local manifest is cleared so that only truly nested calls are recorded.
     *     Subsequent enqueues will start a new manifest
     */
    class ChannelManifest
    {
    public:
        /**
         * Records a call to enqueue, with an optional delay
         * @param name  The name of the method being enqueued
         * @param after The delay, if any, that the method will be delayed before execution
         */
        void addEnqueueCall(const litecore::actor::Actor* actor, const char* name, double after = 0.0);

        /**
         * Records an execution of a previously queued item
         * @param name  The name of the method that will be executed
         */
        void addExecution(const litecore::actor::Actor* actor, const char* name);

        /**
         * Records the history of this manifest to the given output stream.  The format is:
         *
         *     List of enqueue calls:
         *         [xx ms] function::name
         *         [xx ms] function::name
         *         ...
         *     Resulting execution calls:
         *         [xx ms] function::name
         *         [xx ms] function::name
         *         ...
         *
         * "xx ms" is the number of milliseconds since the manifest started
         *
         * @param out   The stream to write the result to
         */
        void dump(std::ostream& out);

        /**
         * Sets the number of "frames" to keep track of to avoid unbounded growth
         */
        void setLimit(uint8_t limit) {
            _limit = limit;
        }
    private:
        struct ChannelManifestEntry
        {
            std::chrono::microseconds elapsed;
            std::string description;
        };

        const std::chrono::system_clock::time_point _start = std::chrono::system_clock::now();

        std::list<ChannelManifestEntry> _enqueueCalls;
        std::list<ChannelManifestEntry> _executions;
        uint8_t _limit {100};
        uint32_t _truncatedEnqueue {0};
        uint32_t _truncatedExecution {0};
        std::mutex _mutex;
    };

}

#endif
