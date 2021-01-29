//
// ThreadedMailbox.hh
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
#include "Channel.hh"
#include "ChannelManifest.hh"
#include "RefCounted.hh"
#include "Stopwatch.hh"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <functional>
#include <vector>

namespace litecore { namespace actor {
    using fleece::RefCounted;
    using fleece::Retained;

    class Scheduler;
    class Actor;
    class MailboxProxy;


    /** A delay expressed in floating-point seconds */
    using delay_t = std::chrono::duration<double>;


    #ifndef ACTORS_USE_GCD
    /** Default Actor mailbox implementation that uses a thread pool run by a Scheduler. */
    class ThreadedMailbox : Channel<std::function<void()>> {
    public:
        ThreadedMailbox(Actor*, const std::string &name ="", ThreadedMailbox *parentMailbox =nullptr);

        const std::string& name() const                     {return _name;}

        unsigned eventCount() const                         {return (unsigned)size() + (unsigned)_delayedEventCount;}

        void enqueue(const char* name, const std::function<void()>&);
        void enqueueAfter(delay_t delay, const char* name, const std::function<void()>&);

        static Actor* currentActor()                        {return sCurrentActor;}

        static void runAsyncTask(void (*task)(void*), void *context);

        void logStats() const;

    private:
        friend class Scheduler;
        
        void reschedule();
        void performNextMessage();
        void afterEvent();
        void safelyCall(const std::function<void()> &f) const;

        Actor* const _actor;
        std::string const _name;

        int _delayedEventCount {0};
#if DEBUG
        std::atomic_int _active {0};
#endif

#if ACTORS_TRACK_STATS
        int32_t _callCount {0};
        int32_t _maxEventCount {0};
        double _maxLatency {0};
        fleece::Stopwatch _createdAt {true};
        fleece::Stopwatch _busy {false};
#endif
        
        static thread_local Actor* sCurrentActor;

#if ACTORS_USE_MANIFESTS
        mutable ChannelManifest _localManifest;
        static thread_local std::shared_ptr<ChannelManifest> sThreadManifest;
#endif
    };

    /** The Scheduler is reponsible for calling ThreadedMailboxes to run their Actor methods.
        It managers a thread pool on which Mailboxes and Actors will run. */
    class Scheduler {
    public:
        Scheduler(unsigned numThreads =0)
        :_numThreads(numThreads)
        { }

        /** Returns a per-process shared instance. */
        static Scheduler* sharedScheduler();

        /** Starts the background threads that will run queued Actors. */
        void start();

        /** Stops the background threads. Blocks until all pending messages are handled. */
        void stop();

        /** Runs the scheduler on the current thread; doesn't return until all pending
            messages are handled. */
        void runSynchronous()                               {task(0);}

    protected:
        friend class ThreadedMailbox;

        /** A request for an Actor's performNextMessage method to be called. */
        static void schedule(ThreadedMailbox* mbox);

    private:
        void task(unsigned taskID);

        unsigned _numThreads;
        Channel<ThreadedMailbox*> _queue;
        std::vector<std::thread> _threadPool;
        std::atomic_flag _started = ATOMIC_FLAG_INIT;


    };

    // This prevents the compiler from specializing Channel in every compilation unit:
    extern template class Channel<ThreadedMailbox*>;
    extern template class Channel<std::function<void()>>;
#endif

} }
