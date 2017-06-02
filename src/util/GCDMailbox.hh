//
//  GCDMailbox.hh
//  blip_cpp
//
//  Created by Jens Alfke on 4/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "ThreadedMailbox.hh"
#include "Stopwatch.hh"
#include <functional>
#include <string>
#include <dispatch/dispatch.h>

namespace litecore { namespace actor {
    class Actor;

    /** Actor mailbox that uses a Grand Central Dispatch (GCD) serial dispatch_queue.
        Available on Apple platforms, or elsewhere if libdispatch is installed. */
    class GCDMailbox {
    public:
        GCDMailbox(Actor *a, const std::string &name ="", Scheduler *s =nullptr);
        ~GCDMailbox();

        std::string name() const;

        Scheduler* scheduler() const                        {return nullptr;}
        void setScheduler(Scheduler *s)                     { }

        unsigned eventCount() const                         {return _eventCount;}

        //void enqueue(std::function<void()> f);
        void enqueue(void (^block)());
        void enqueueAfter(delay_t delay, void (^block)());

        static void startScheduler(Scheduler *)             { }

        void logStats();

        static GCDMailbox* currentMailbox();
        static Actor* currentActor();

    private:
        void afterEvent();
        
        Actor *_actor;
        dispatch_queue_t _queue;
        std::atomic<int32_t> _eventCount {0};
        
#if ACTORS_TRACK_STATS
        int32_t _maxEventCount {0};
        double _maxLatency {0};
        fleece::Stopwatch _createdAt {true};
        fleece::Stopwatch _busy {false};
#endif
    };

} }
