//
// ThreadedMailbox.hh
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
#include "Channel.hh"
#include "ChannelManifest.hh"
#include "fleece/RefCounted.hh"
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

        const Actor* actor() const { return _actor; }

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
