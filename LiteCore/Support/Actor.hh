//
// Actor.hh
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
#include "ThreadedMailbox.hh"
#include "Logging.hh"
#include <assert.h>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

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

        template <class T>
        auto asynchronize(const char* methodName, T t) -> decltype(get_fun_type(&T::operator())) {
            decltype(get_fun_type(&T::operator())) fn = t;
            return _asynchronize(methodName, fn);
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
