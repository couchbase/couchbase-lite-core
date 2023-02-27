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

namespace litecore { namespace actor {


    bool AsyncState::_asyncCall(const AsyncBase &a, int lineNo) {
        _calling    = a._context;
        _continueAt = lineNo;
        return !a.ready();
    }


#if DEBUG
    std::atomic_int AsyncContext::gInstanceCount;
#endif


    AsyncContext::AsyncContext(Actor *actor) : _actor(actor) {
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

    void AsyncContext::start() {
        _waitingSelf = this;
        if ( _actor && _actor != Actor::currentActor() ) _actor->wakeAsyncContext(this);  // Start on my Actor's queue
        else
            next();
    }

    void AsyncContext::_wait() {
        _waitingActor = Actor::currentActor();  // retain my actor while I'm waiting
        _calling->setObserver(this);
    }

    void AsyncContext::wakeUp(AsyncContext *async) {
        assert(async == _calling);
        if ( _waitingActor ) {
            fleece::Retained<Actor> waitingActor = std::move(_waitingActor);
            waitingActor->wakeAsyncContext(this);  // queues the next() call on its Mailbox
        } else {
            next();
        }
    }

    void AsyncContext::_gotResult() {
        _ready        = true;
        auto observer = _observer;
        _observer     = nullptr;
        if ( observer ) observer->wakeUp(this);
        _waitingSelf = nullptr;
    }

}}  // namespace litecore::actor
