//
// GCDMailbox.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
#include "ThreadedMailbox.hh"
#include "Stopwatch.hh"
#include "ChannelManifest.hh"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <dispatch/dispatch.h>

namespace litecore { namespace actor {
    class Actor;

    /** Actor mailbox that uses a Grand Central Dispatch (GCD) serial dispatch_queue.
        Available on Apple platforms, or elsewhere if libdispatch is installed. */
    class GCDMailbox {
    public:
        GCDMailbox(Actor *a, const std::string &name ="", GCDMailbox *parentMailbox =nullptr);
        ~GCDMailbox();

        std::string name() const;

        Scheduler* scheduler() const                        {return nullptr;}
        void setScheduler(Scheduler *s)                     { }

        unsigned eventCount() const                         {return _eventCount;}

        //void enqueue(std::function<void()> f);
        void enqueue(const char* name, void (^block)());
        void enqueueAfter(delay_t delay, const char* name, void (^block)());

        static void startScheduler(Scheduler *)             { }

        void logStats() const;

        static Actor* currentActor();

        static void runAsyncTask(void (*task)(void*), void *context);

    private:
        void runEvent(void (^block)());
        void afterEvent();
        void safelyCall(void (^block)()) const;
        
        Actor *_actor;
        dispatch_queue_t _queue;
        std::atomic<int32_t> _eventCount {0};
        
#if ACTORS_USE_MANIFESTS
        mutable ChannelManifest _localManifest;
        static thread_local std::shared_ptr<ChannelManifest> sQueueManifest;
#endif
        
#if ACTORS_TRACK_STATS
        int32_t _callCount {0};
        int32_t _maxEventCount {0};
        double _maxLatency {0};
        double _maxBusy {0};
        fleece::Stopwatch _createdAt {true};
        fleece::Stopwatch _busy {false};
#endif
    };

} }
