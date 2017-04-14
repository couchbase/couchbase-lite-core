//
//  ThreadedMailbox.hh
//  blip_cpp
//
//  Created by Jens Alfke on 4/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Channel.hh"
#include "RefCounted.hh"
#include <atomic>
#include <string>
#include <thread>

#define ACTORS_TRACK_STATS  DEBUG

namespace litecore { namespace actor {
    class Scheduler;
    class Actor;
    class MailboxProxy;


    /** A delay expressed in floating-point seconds */
    using delay_t = std::chrono::duration<double>;


    /** Default Actor mailbox implementation that uses a thread pool run by a Scheduler. */
    class ThreadedMailbox : Channel<std::function<void()>> {
    public:
        ThreadedMailbox(Actor*, const std::string &name ="");
        ~ThreadedMailbox();

        const std::string& name() const                     {return _name;}

        unsigned eventCount() const                         {return (unsigned)size();}

        void enqueue(std::function<void()>);
        void enqueueAfter(delay_t delay, std::function<void()>);

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
