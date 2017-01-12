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
#include <assert.h>
#include <functional>
#include <string>
#include <thread>

namespace litecore {

    class Actor;


    /** The Scheduler is reponsible for calling Actors.
        It managers a thread pool on which Actor methods will run. */
    class Scheduler {
    public:

        /** Starts the background threads that will run queued Actors. */
        void start(unsigned numThreads =0);

        /** Stops the background threads. Blocks until all pending messages are handled. */
        void stop();

        /** Runs the scheduler on the current thread; doesn't return until all pending
            messages are handled. */
        void runSynchronous()                               {task(0);}

    protected:
        friend class Actor;

        /** A request for an Actor's performNextMessage method to be called. */
        void schedule(Retained<Actor> actor)                {_queue.push(actor);}

    private:
        void task(unsigned taskID);

        Channel<Retained<Actor>> _queue;
        std::vector<std::thread> _threadPool;
    };



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

        Scheduler* scheduler() const                        {return _scheduler;}
        void setScheduler(Scheduler *s);

    protected:
        Actor() { }

        Actor(Scheduler *sched)
        :_scheduler(sched)
        { }

        template <class Rcvr, class... Args>
        void enqueue(void (Rcvr::*fn)(Args...), Args... args) {
            enqueue(std::bind(fn, (Rcvr*)this, args...));
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

        void reschedule()                                   {_scheduler->schedule(this);}
        void enqueue(std::function<void()> f);
        void performNextMessage();

        Channel<std::function<void()>> _mailbox;
        Scheduler *_scheduler {nullptr};
    };


    // This prevents the compiler from specializing Channel in every compilation unit:
    extern template class Channel<Retained<Actor>>;
    extern template class Channel<std::function<void()>>;

}
