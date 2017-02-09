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
#include <functional>
#include <string>
#include <thread>

#if __APPLE__
#define ACTORS_USE_GCD
#endif

#ifdef ACTORS_USE_GCD
#include <dispatch/dispatch.h>
#endif

namespace litecore {

    class Actor;
    class ThreadedMailbox;


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
        void schedule(ThreadedMailbox* mbox)                {_queue.push(mbox);}

    private:
        void task(unsigned taskID);

        unsigned _numThreads;
        Channel<ThreadedMailbox*> _queue;
        std::vector<std::thread> _threadPool;
        std::atomic_flag _started;
    };



    /** Default Actor mailbox implementation that uses a thread pool run by a Scheduler. */
    class ThreadedMailbox : Channel<std::function<void()>> {
    public:
        ThreadedMailbox(Actor *a, const std::string &name ="", Scheduler *s =nullptr)
        :_actor(a)
        ,_name(name)
        ,_scheduler(s)
        { }

        Scheduler* scheduler() const                        {return _scheduler;}
        void setScheduler(Scheduler *s);

        void enqueue(std::function<void()> f);

        static void startScheduler(Scheduler *s)            {s->start();}

    private:
        friend class Scheduler;
        
        void reschedule()                                   {_scheduler->schedule(this);}
        void performNextMessage();

        Actor* const _actor;
        std::string const _name;
        Scheduler *_scheduler {nullptr};
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

        Scheduler* scheduler() const                        {return nullptr;}
        void setScheduler(Scheduler *s)                     { }

        void enqueue(std::function<void()> f);

        void enqueue(void (^block)()) {
            dispatch_async(_queue, block);
        }

        static void startScheduler(Scheduler *)             { }

    private:
        Actor *_actor;
        dispatch_queue_t _queue;
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

        Scheduler* scheduler() const                        {return _mailbox.scheduler();}
        void setScheduler(Scheduler *s)                     {_mailbox.setScheduler(s);}

    protected:
        Actor(const std::string &name ="", Scheduler *sched =nullptr)
        :_mailbox(this, name, sched)
        { }

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

        /** Converts a lambda into a form that runs asynchronously,
            i.e. when called it schedules a call of the orignal lambda on the actor's thread.
            Use this when registering callbacks, e.g. with a Future.*/
        template <class Arg>
        std::function<void(Arg)> asynchronize(std::function<void(Arg)> fn) {
            Retained<Actor> ret(this);
            return [=](Arg arg) mutable {
                ret->_mailbox.enqueue( std::bind(fn, arg) );
                ret = nullptr;
            };
        }

        /** Convenience function for creating a callback on a Future. */
        template <class T, class LAMBDA>
        void onReady(Retained<Future<T>> future, LAMBDA callback) {
            std::function<void(T)> fn(callback);
            future->onReady( asynchronize(fn) );
        }

        template <class T>
        class PropertyImpl {
        public:
            PropertyImpl(Actor *owner)              :_owner(*owner) { }
            PropertyImpl(Actor *owner, T t)         :_owner(*owner), _value(t) { }

            T get() const                           {return _value;}
            operator T() const                      {return _value;}
            PropertyImpl& operator= (const T &t)    {_value = t; return *this;}
        private:
            Actor &_owner;
            T _value {};
        };


        template <class T>
        class Property {
        public:
            Property(PropertyImpl<T> &prop)     :_impl(prop) { }

//            void addObserver(Actor &a, ObservedProperty<T> &observer);
            void removeObserver(Actor &a);
        private:
            PropertyImpl<T> &_impl;
            std::vector<Retained<Actor>> _observers;
        };

        template <class T>
        class ObservedProperty {
        public:
            T get() const                           {return _value;}
            operator T() const                      {return _value;}
        private:
            Retained<Actor> &_provider;
            T _value;
        };

    private:
        friend class Scheduler;

        Mailbox _mailbox;
    };


    // This prevents the compiler from specializing Channel in every compilation unit:
    extern template class Channel<ThreadedMailbox*>;
    extern template class Channel<std::function<void()>>;

}
