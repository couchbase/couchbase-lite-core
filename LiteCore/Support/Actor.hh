//
// Actor.hh
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
#include "Logging.hh"
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <betterassert.hh>

#ifdef ACTORS_USE_GCD
#include "GCDMailbox.hh"
#endif

#ifdef ACTORS_TRACK_STATS
#include "Stopwatch.hh"
#endif

#ifdef ACTORS_SUPPORT_ASYNC
#include "Async.hh"
#endif


namespace litecore { namespace actor {
    class Actor;
    class AsyncContext;


    //// Some support code for asynchronize(), from http://stackoverflow.com/questions/42124866
    template <class RetVal, class T, class... Args>
    std::function<RetVal(Args...)> get_fun_type(RetVal (T::*)(Args...) const);

    template <class RetVal, class T, class... Args>
    std::function<RetVal(Args...)> get_fun_type(RetVal (T::*)(Args...));

    template <class RetVal, class... Args>
    std::function<RetVal(Args...)> get_fun_type(RetVal (*)(Args...));
    ////


#ifdef ACTORS_USE_GCD
    using Mailbox = GCDMailbox;
    #define ACTOR_BIND_METHOD0(RCVR, METHOD)        ^{ ((RCVR)->*METHOD)(); }
    #define ACTOR_BIND_METHOD(RCVR, METHOD, ARGS)   ^{ ((RCVR)->*METHOD)(ARGS...); }
    #define ACTOR_BIND_FN(FN, ARGS)                 ^{ FN(ARGS...); }
#else
    using Mailbox = ThreadedMailbox;
    #define ACTOR_BIND_METHOD0(RCVR, METHOD)        std::bind(METHOD, RCVR)
    #define ACTOR_BIND_METHOD(RCVR, METHOD, ARGS)   std::bind(METHOD, RCVR, ARGS...)
    #define ACTOR_BIND_FN(FN, ARGS)                 std::bind(FN, ARGS...)
#endif

    #define FUNCTION_TO_QUEUE(METHOD) #METHOD, &METHOD


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
    class Actor : public RefCounted, public Logging {
    public:

        unsigned eventCount() const                         {return _mailbox.eventCount();}

        std::string actorName() const                       {return _mailbox.name();}

        /** The Actor that's currently running, else nullptr */
        static Actor* currentActor()                        {return Mailbox::currentActor();}

        /** Blocks until the Actor has finished handling all outstanding events.
            Obviously the actor should never call this on itself, nor should it be called by
            anything else that might be called directly by the actor (on its thread.) */
        void waitTillCaughtUp();

        template <class T>
        auto asynchronize(const char* methodName, T t) -> decltype(get_fun_type(&T::operator())) {
            decltype(get_fun_type(&T::operator())) fn = t;
            return _asynchronize(methodName, fn);
        }

    protected:
        /** Constructs an Actor.
            @param domain The domain which this actor is logged to.
            @param name  Used for logging, and on Apple platforms for naming the GCD queue;
                        otherwise unimportant.
            @param parentMailbox  Used for limiting concurrency on some platforms: if non-null,
                        then only one Actor with the same parentMailbox can execute at once.
                        This helps control the number of threads created by the OS. This is only
                        implemented on Apple platforms, where it determines the target queue. */
        Actor(LogDomain& domain, const std::string &name ="", Mailbox *parentMailbox =nullptr)
        :Logging(domain)
        ,_mailbox(this, name, parentMailbox)
        { }

        /** Schedules a call to a method. */
        template <class Rcvr, class... Args>
        void enqueue(const char* methodName, void (Rcvr::*fn)(Args...), Args... args) {
            _mailbox.enqueue(methodName, ACTOR_BIND_METHOD((Rcvr*)this, fn, args));
        }

        /** Schedules a call to a method, after a delay.
            Other calls scheduled after this one may end up running before it! */
        template <class Rcvr, class... Args>
        void enqueueAfter(delay_t delay, const char* methodName, void (Rcvr::*fn)(Args...), Args... args) {
            _mailbox.enqueueAfter(delay, methodName, ACTOR_BIND_METHOD((Rcvr*)this, fn, args));
        }

        /** Converts a lambda into a form that runs asynchronously,
            i.e. when called it schedules a call of the orignal lambda on the actor's thread.
            Use this when registering callbacks, e.g. with a Future.*/
        template <class... Args>
        std::function<void(Args...)> _asynchronize(const char* methodName, std::function<void(Args...)> fn) {
            Retained<Actor> ret(this);
            return [=](Args ...arg) mutable {
                ret->_mailbox.enqueue(methodName, ACTOR_BIND_FN(fn, arg));
            };
        }

        virtual void afterEvent()                    { }

        virtual void caughtException(const std::exception &x);

        virtual std::string loggingIdentifier() const { return actorName(); }

        void logStats() {
            _mailbox.logStats();
        }


#ifdef ACTORS_SUPPORT_ASYNC
        /** Body of an async method: Creates an Provider from the lambda given,
            then returns an Async that refers to that provider. */
        template <class T, class LAMBDA>
        Async<T> _asyncBody(const LAMBDA &bodyFn) {
            return Async<T>(this, bodyFn);
        }

        void wakeAsyncContext(AsyncContext *context) {
            _mailbox.enqueue(ACTOR_BIND_METHOD0(context, &AsyncContext::next));
        }
#endif

    private:
        friend class ThreadedMailbox;
        friend class GCDMailbox;
        friend class AsyncContext;

        template <class ACTOR, class ITEM> friend class ActorBatcher;
        template <class ACTOR>             friend class ActorCountBatcher;

        void _waitTillCaughtUp(std::mutex*, std::condition_variable*, bool*);

        Mailbox _mailbox;
    };


#undef ACTOR_BIND_METHOD
#undef ACTOR_BIND_FN

} }
