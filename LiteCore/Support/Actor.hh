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
#include "AsyncActorCommon.hh"
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


namespace litecore::actor {

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
    #define ACTOR_BIND_FN0(FN)                      ^{ FN(); }
#else
    using Mailbox = ThreadedMailbox;
    #define ACTOR_BIND_METHOD0(RCVR, METHOD)        std::bind(METHOD, RCVR)
    #define ACTOR_BIND_METHOD(RCVR, METHOD, ARGS)   std::bind(METHOD, RCVR, ARGS...)
    #define ACTOR_BIND_FN(FN, ARGS)                 std::bind(FN, ARGS...)
    #define ACTOR_BIND_FN0(FN)                      (FN)
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

        /** Within an Actor method, `thisActor` evaluates to `this`.
            (Outside of one, it calls the static function `thisActor` that returns nullptr.) */
        Actor* thisActor() {return this;}
        const Actor* thisActor() const {return this;}

        /** Returns true if `this` is the currently running Actor. */
        bool isCurrentActor() const                          {return currentActor() == this;}

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

        /** Schedules a call to `fn` on the actor's thread.
            The return type depends on `fn`s return type:
            - `void` -- `asCurrentActor` will return `void`.
            - `X` -- `asCurrentActor` will return `Async<X>`, which will resolve after `fn` runs.
            - `Async<X>` -- `asCurrentActor` will return `Async<X>`, which will resolve after `fn`
                            runs _and_ its returned async value resolves. */
        template <typename LAMBDA>
        auto asCurrentActor(LAMBDA &&fn) {
            using U = unwrap_async<std::invoke_result_t<LAMBDA>>; // return type w/o Async<>
            return _asCurrentActor<U>(std::forward<LAMBDA>(fn));
        }

        /** The scheduler calls this after every call to the Actor. */
        virtual void afterEvent()                    { }

        /** Called if an Actor method throws an exception. */
        virtual void caughtException(const std::exception &x);

        virtual std::string loggingIdentifier() const { return actorName(); }

        /** Writes statistics to the log. */
        void logStats() {
            _mailbox.logStats();
        }

    private:
        friend class ThreadedMailbox;
        friend class GCDMailbox;
        friend class AsyncProviderBase;

        template <class ACTOR, class ITEM> friend class ActorBatcher;
        template <class ACTOR>             friend class ActorCountBatcher;

        void _waitTillCaughtUp(std::mutex*, std::condition_variable*, bool*);

        /** Calls a method on _some other object_ on my mailbox's queue. */
        template <class Rcvr, class... Args>
        void enqueueOther(const char* methodName, Rcvr* other, void (Rcvr::*fn)(Args...), Args... args) {
            _mailbox.enqueue(methodName, ACTOR_BIND_METHOD(other, fn, args));
        }

        // Implementation of `asCurrentActor` where `fn` returns a non-async type `T`.
        template <typename T>
        auto _asCurrentActor(std::function<T()> fn) {
            auto provider = Async<T>::makeProvider();
            asCurrentActor([fn,provider] { provider->setResultFromFunction(fn); });
            return provider->asyncValue();
        }

        // Implementation of `asCurrentActor` where `fn` itself returns an `Async`.
        template <typename U>
        Async<U> _asCurrentActor(std::function<Async<U>()> fn) {
            auto provider = Async<U>::makeProvider();
            asCurrentActor([fn,provider] {
                fn().thenProvider([=](AsyncProvider<U> &fnProvider) {
                    provider->setResult(std::move(fnProvider).result());
                });
            });
            return provider;
        }

        Mailbox _mailbox;
    };


    // Specialization of `asCurrentActor` where `fn` returns void.
    template <>
    inline auto Actor::_asCurrentActor<void>(std::function<void()> fn) {
        if (currentActor() == this)
            fn();
        else
            _mailbox.enqueue("asCurrentActor", ACTOR_BIND_FN0(fn));
    }

#undef ACTOR_BIND_METHOD
#undef ACTOR_BIND_METHOD0
#undef ACTOR_BIND_FN
#undef ACTOR_BIND_FN0

}
