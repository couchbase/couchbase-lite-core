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
