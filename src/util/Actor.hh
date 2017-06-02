//
//  Actor.hh
//  Actors
//
//  Created by Jens Alfke on 1/5/17.
//
//

#pragma once
#include "ThreadedMailbox.hh"
#include "RefCounted.hh"
#include "Future.hh"
#include <assert.h>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#if __APPLE__
// Use GCD if available, as it's more efficient and has better integration with OS & debugger.
#define ACTORS_USE_GCD
#endif

#ifdef ACTORS_USE_GCD
#include "GCDMailbox.hh"
#endif

#ifdef ACTORS_TRACK_STATS
#include "Stopwatch.hh"
#endif


namespace litecore { namespace actor {
    class Actor;


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
    #define ACTOR_BIND_METHOD(RCVR, METHOD, ARGS)   ^{ ((RCVR)->*METHOD)(ARGS...); }
    #define ACTOR_BIND_FN(FN, ARGS)                 ^{ FN(ARGS...); }
#else
    using Mailbox = ThreadedMailbox;
    #define ACTOR_BIND_METHOD(RCVR, METHOD, ARGS)   std::bind(METHOD, RCVR, ARGS...)
    #define ACTOR_BIND_FN(FN, ARGS)                 std::bind(FN, ARGS...)
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
            _mailbox.enqueue(ACTOR_BIND_METHOD((Rcvr*)this, fn, args));
        }

        /** Schedules a call to a method, after a delay.
            Other calls scheduled after this one may end up running before it! */
        template <class Rcvr, class... Args>
        void enqueueAfter(delay_t delay, void (Rcvr::*fn)(Args...), Args... args) {
            _mailbox.enqueueAfter(delay, ACTOR_BIND_METHOD((Rcvr*)this, fn, args));
        }

        /** Converts a lambda into a form that runs asynchronously,
            i.e. when called it schedules a call of the orignal lambda on the actor's thread.
            Use this when registering callbacks, e.g. with a Future.*/
        template <class... Args>
        std::function<void(Args...)> _asynchronize(std::function<void(Args...)> fn) {
            Retained<Actor> ret(this);
            return [=](Args ...arg) mutable {
                ret->_mailbox.enqueue(ACTOR_BIND_FN(fn, arg));
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
        friend class GCDMailbox;

        Mailbox _mailbox;
    };


#undef ACTOR_BIND_METHOD
#undef ACTOR_BIND_FN

} }
