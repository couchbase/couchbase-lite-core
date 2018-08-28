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
#include "RefCounted.hh"
#include <atomic>
#include <string>
#include <thread>
#include <functional>

// Set to 1 to have Actor object report performance statistics in their destructors
#define ACTORS_TRACK_STATS  0

namespace litecore { namespace actor {
    using fleece::RefCounted;
    using fleece::Retained;

    class Scheduler;
    class Actor;
    class MailboxProxy;


    /** A delay expressed in floating-point seconds */
    using delay_t = std::chrono::duration<double>;


    /** Default Actor mailbox implementation that uses a thread pool run by a Scheduler. */
    class ThreadedMailbox : Channel<std::function<void()>> {
    public:
        ThreadedMailbox(Actor*, const std::string &name ="", ThreadedMailbox *parentMailbox =nullptr);
        ~ThreadedMailbox();

        const std::string& name() const                     {return _name;}

        unsigned eventCount() const                         {return (unsigned)size();}

        void enqueue(std::function<void()>);
        void enqueueAfter(delay_t delay, std::function<void()>);

        static Actor* currentActor()                        {return sCurrentActor;}

        void logStats()                                     { /* TODO */ }

    private:
        friend class Scheduler;
        
        void reschedule();
        void performNextMessage();

        Actor* const _actor;
        std::string const _name;
        Retained<MailboxProxy> _proxy;
#if DEBUG
        std::atomic_int _active {0};
#endif
        
        static __thread Actor* sCurrentActor;
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

} }
