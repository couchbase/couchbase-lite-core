//
//  Actor.hh
//  Actors
//
//  Created by Jens Alfke on 1/5/17.
//
//

#pragma once
#include "Channel.hh"
#include "RefCounted.hh"
#include "Future.hh"
#include <assert.h>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#if __APPLE__
#define ACTORS_USE_GCD
#endif

#ifdef ACTORS_USE_GCD
#include <dispatch/dispatch.h>
#endif

#define ACTORS_TRACK_STATS  DEBUG

#if ACTORS_TRACK_STATS
#include "Benchmark.hh"
#endif


namespace litecore {

    class Actor;
    class ThreadedMailbox;


    //// Some support code for asynchronize(), from http://stackoverflow.com/questions/42124866
    template <class RetVal, class T, class... Args>
    std::function<RetVal(Args...)> get_fun_type(RetVal (T::*)(Args...) const);

    template <class RetVal, class T, class... Args>
    std::function<RetVal(Args...)> get_fun_type(RetVal (T::*)(Args...));

    template <class RetVal, class... Args>
    std::function<RetVal(Args...)> get_fun_type(RetVal (*)(Args...));
    ////


    /** The Scheduler is reponsible for calling ThreadedMailboxes to run their Actor methods.
        It managers a thread pool on which Mailboxes and Actors will run. */
    class Scheduler {
    public:
        using duration = std::chrono::duration<double>; // time duration in floating-point seconds

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
        void schedule(ThreadedMailbox* mbox)                {_queue.push(mbox);}

    private:
        void task(unsigned taskID);

        unsigned _numThreads;
        Channel<ThreadedMailbox*> _queue;
        std::vector<std::thread> _threadPool;
        std::atomic_flag _started = ATOMIC_FLAG_INIT;
    };



    /** Default Actor mailbox implementation that uses a thread pool run by a Scheduler. */
    class ThreadedMailbox : Channel<std::function<void()>> {
    public:
        ThreadedMailbox(Actor*, const std::string &name ="");

        const std::string& name() const                     {return _name;}

        unsigned eventCount() const                         {return (unsigned)size();}

        void enqueue(std::function<void()>);
        void enqueueAfter(Scheduler::duration delay, std::function<void()>);

        void logStats()                                     { /* TODO */ }

    private:
        friend class Scheduler;
        
        void reschedule();
        void performNextMessage();

        Actor* const _actor;
        std::string const _name;
#if DEBUG
        std::atomic_int _active {0};
#endif
    };


#ifdef ACTORS_USE_GCD
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

        void enqueue(std::function<void()> f);
        void enqueue(void (^block)());
        void enqueueAfter(Scheduler::duration delay, std::function<void()>);
        void enqueueAfter(Scheduler::duration delay, void (^block)());

        static void startScheduler(Scheduler *)             { }

        void logStats();

    private:
        void afterEvent();
        
        Actor *_actor;
        dispatch_queue_t _queue;
        std::atomic<int32_t> _eventCount {0};
#if ACTORS_TRACK_STATS
        int32_t _maxEventCount {0};
        double _maxLatency {0};
        Stopwatch _createdAt {true};
        Stopwatch _busy {false};
#endif
    };
#endif


    // Use GCD if available, as it's more efficient and has better integration with OS & debugger.
#ifdef ACTORS_USE_GCD
    typedef GCDMailbox Mailbox;
#else
    typedef ThreadedMailbox Mailbox;
#endif



    /** Abstract base actor class. Subclasses should implement their public methods as calls to
        `enqueue` that pass the parameter values through, and name a matching private 
        implementation method; for example:
            class Adder : public Actor {
                public:  void add(int a, bool clear)        {enqueue(&Adder::_add, a, clear);}
                private: void _add(int a, bool clear)       {... actual implementation...}
            };
        The public method will return immediately; the private one will be called later (on
        a private thread belonging to the Scheduler). It is guaranteed that only one enqueued
        method call will be run at once, so the Actor implementation is effectively single-
        threaded. */
    class Actor : public RefCounted {
    public:

        unsigned eventCount() const                         {return _mailbox.eventCount();}

        std::string actorName() const                       {return _mailbox.name();}

    protected:
        Actor(const std::string &name ="")
        :_mailbox(this, name)
        { }

        /** Schedules a call to a method. */
        template <class Rcvr, class... Args>
        void enqueue(void (Rcvr::*fn)(Args...), Args... args) {
#ifdef ACTORS_USE_GCD
            // not strictly necessary, but more efficient
            retain(this);
            _mailbox.enqueue( ^{ (((Rcvr*)this)->*fn)(args...); release(this); } );
#else
            _mailbox.enqueue(std::bind(fn, (Rcvr*)this, args...));
#endif
        }

        using delay_t = Scheduler::duration;

        /** Schedules a call to a method, after a delay.
            Other calls scheduled after this one may end up running before it! */
        template <class Rcvr, class... Args>
        void enqueueAfter(delay_t delay, void (Rcvr::*fn)(Args...), Args... args) {
#ifdef ACTORS_USE_GCD
            // not strictly necessary, but more efficient
            retain(this);
            _mailbox.enqueueAfter(delay, ^{ (((Rcvr*)this)->*fn)(args...); release(this); } );
#else
            _mailbox.enqueueAfter(delay, std::bind(fn, (Rcvr*)this, args...));
#endif
        }

        /** Converts a lambda into a form that runs asynchronously,
            i.e. when called it schedules a call of the orignal lambda on the actor's thread.
            Use this when registering callbacks, e.g. with a Future.*/
        template <class... Args>
        std::function<void(Args...)> _asynchronize(std::function<void(Args...)> fn) {
            Retained<Actor> ret(this);
            return [=](Args ...arg) mutable {
                ret->_mailbox.enqueue( std::bind(fn, arg...) );
            };
        }

        template <class T>
        auto asynchronize(T t) -> decltype(get_fun_type(&T::operator())) {
            decltype(get_fun_type(&T::operator())) fn = t;
            return _asynchronize(fn);
        }

        /** Convenience function for creating a callback on a Future. */
        template <class T, class LAMBDA>
        void onReady(Retained<Future<T>> future, LAMBDA callback) {
            std::function<void(T)> fn(callback);
            future->onReady( asynchronize(fn) );
        }

        virtual void afterEvent()                    { }

        void logStats() {
            _mailbox.logStats();
        }

    private:
        friend class ThreadedMailbox;
#ifdef ACTORS_USE_GCD
        friend class GCDMailbox;
#endif

        Mailbox _mailbox;
    };


    // This prevents the compiler from specializing Channel in every compilation unit:
    extern template class Channel<ThreadedMailbox*>;
    extern template class Channel<std::function<void()>>;

}
