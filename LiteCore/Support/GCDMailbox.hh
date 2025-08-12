//
// GCDMailbox.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "ThreadedMailbox.hh"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <dispatch/dispatch.h>

namespace litecore::actor {
    class Actor;

    /** Actor mailbox that uses a Grand Central Dispatch (GCD) serial dispatch_queue.
        Available on Apple platforms, or elsewhere if libdispatch is installed. */
    class GCDMailbox {
      public:
        explicit GCDMailbox(Actor* a, const std::string& name = "", GCDMailbox* parentMailbox = nullptr);
        ~GCDMailbox();

        std::string name() const;

        unsigned eventCount() const { return _eventCount; }

        //void enqueue(std::function<void()> f);
        void enqueue(const char* name, void (^block)());
        void enqueueAfter(delay_t delay, const char* name, void (^block)());

        void logStats() const;

        static Actor* currentActor();

        dispatch_queue_t dispatchQueue() const noexcept { return _queue; }

        static void runAsyncTask(void (*task)(void*), void* context);

      private:
        void runEvent(void (^block)());
        void afterEvent();
        void safelyCall(void (^block)()) const;

        Actor*               _actor;
        dispatch_queue_t     _queue;
        std::atomic<int32_t> _eventCount{0};

#if ACTORS_USE_MANIFESTS
        mutable ChannelManifest                              _localManifest;
        static thread_local std::shared_ptr<ChannelManifest> sQueueManifest;
#endif

#if ACTORS_TRACK_STATS
        int32_t           _callCount{0};
        int32_t           _maxEventCount{0};
        double            _maxLatency{0};
        double            _maxBusy{0};
        fleece::Stopwatch _createdAt{true};
        fleece::Stopwatch _busy{false};
#endif
    };

}  // namespace litecore::actor
