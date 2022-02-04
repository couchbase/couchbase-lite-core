//
// Async.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Async.hh"
#include "Actor.hh"
#include "betterassert.hh"

namespace litecore::actor {


    void AsyncState::asyncVoidFn(Actor *actor, std::function<void(AsyncState&)> body) {
        if (actor && actor != Actor::currentActor()) {
            // Need to run this on the Actor's queue, so schedule it:
            auto provider = retained(new AsyncProvider<void>(actor, std::move(body)));
            provider->_start();
        } else {
            // It's OK to call the body synchronously. As an optimization, pass it a plain
            // stack-based AsyncState instead of a heap-allocated AsyncProvider:
            AsyncState state;
            body(state);
            if (state._awaiting) {
                // Body didn't finish, is "blocked" in an AWAIT(), so now set up a proper context:
                auto provider = retained(new AsyncProvider<void>(actor,
                                                                 std::move(body),
                                                                 std::move(state)));
                provider->_wait();
            }
        }
    }

    
    bool AsyncState::_await(const AsyncBase &a, int curLine) {
        _awaiting = a._context;
        _currentLine = curLine;
        return !a.ready();
    }


    void AsyncContext::setObserver(AsyncContext *p) {
        precondition(!_observer);
        _observer = p;
    }


    void AsyncContext::_start() {
        _waitingSelf = this;                    // Retain myself while waiting
        if (_actor && _actor != Actor::currentActor())
            _actor->wakeAsyncContext(this);     // Schedule the body to run on my Actor's queue
        else
            _next();                            // or run it synchronously
    }


    // AsyncProvider<T> overrides this, and calls it last.
    // Async<T>::AsyncWaiter overrides this and doesn't call it at all.
    void AsyncContext::_next() {
        if (_awaiting)
            _wait();
        else
            _gotResult();
    }


    void AsyncContext::_wait() {
        _waitingActor = Actor::currentActor();  // retain the actor that's waiting
        _awaiting->setObserver(this);
    }


    void AsyncContext::_waitOn(AsyncContext *context) {
        assert(!_awaiting);
        _waitingSelf = this; // retain myself while waiting
        _awaiting = context;
        _wait();
    }


    void AsyncContext::wakeUp(AsyncContext *async) {
        assert(async == _awaiting);
        if (_waitingActor) {
            fleece::Retained<Actor> waitingActor = std::move(_waitingActor);
            waitingActor->wakeAsyncContext(this);       // queues the next() call on its Mailbox
        } else {
            _next();
        }
    }


    void AsyncContext::_gotResult() {
        _ready = true;
        if (auto observer = move(_observer))
            observer->wakeUp(this);
        _waitingSelf = nullptr; // release myself now that I'm done
    }


    AsyncBase::AsyncBase(AsyncContext *context, bool)
    :AsyncBase(context)
    {
        _context->_start();
    }

}
