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

    bool AsyncState::_asyncCall(const AsyncBase &a, int curLine) {
        _awaiting = a._context;
        _currentLine = curLine;
        return !a.ready();
    }


#if DEBUG
    std::atomic_int AsyncContext::gInstanceCount;
#endif


    AsyncContext::AsyncContext(Actor *actor)
    :_actor(actor)
    {
#if DEBUG
        ++gInstanceCount;
#endif
    }

    AsyncContext::~AsyncContext() {
#if DEBUG
        --gInstanceCount;
#endif
    }

    void AsyncContext::setObserver(AsyncContext *p) {
        assert(!_observer);
        _observer = p;
    }

    void AsyncContext::_start() {
        _waitingSelf = this;
        if (_actor && _actor != Actor::currentActor())
            _actor->wakeAsyncContext(this);     // Start on my Actor's queue
        else
            _next();
    }

    void AsyncContext::_wait() {
        _waitingActor = Actor::currentActor();  // retain my actor while I'm waiting
        _awaiting->setObserver(this);
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
        auto observer = move(_observer);
        if (observer)
            observer->wakeUp(this);
        _waitingSelf = nullptr;
    }

}
