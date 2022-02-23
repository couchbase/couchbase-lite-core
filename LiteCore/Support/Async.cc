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
#include "Logging.hh"
#include "betterassert.hh"

namespace litecore::actor {
    using namespace std;

#pragma mark - ASYNC FN STATE:


    // Called from `BEGIN_ASYNC()`. This is an optimization for a void-returning function that
    // avoids allocating an AsyncProvider if the function body never has to block.
    void AsyncFnState::asyncVoidFn(Actor *thisActor, function<void(AsyncFnState&)> &&fnBody) {
        if (thisActor && thisActor != Actor::currentActor()) {
            // Need to run this on the Actor's queue, so schedule it:
            (new AsyncProvider<void>(thisActor, move(fnBody)))->_start();
        } else {
            // It's OK to call the body synchronously. As an optimization, call it directly with a
            // stack-based AsyncFnState, instead of from a heap-allocated AsyncProvider:
            AsyncFnState state(nullptr, thisActor);
            fnBody(state);
            if (state._awaiting) {
                // Body didn't finish (is "blocked" in an `AWAIT()`), so set up a proper provider:
                (new AsyncProvider<void>(thisActor, move(fnBody), move(state)))->_wait();
            }
        }
    }


    AsyncFnState::AsyncFnState(AsyncProviderBase *owningProvider, Actor *owningActor)
    :_owningProvider(owningProvider)
    ,_owningActor(owningActor)
    { }


    // copy state from `other`, except `_owningProvider`
    void AsyncFnState::updateFrom(AsyncFnState &other) {
        _owningActor = other._owningActor;
        _awaiting = move(other._awaiting);
        _currentLine = other._currentLine;
    }


    // called by `AWAIT()` macro
    bool AsyncFnState::_await(const AsyncBase &a, int curLine) {
        _awaiting = a._provider;
        _currentLine = curLine;
        return !a.ready();
    }


#pragma mark - ASYNC OBSERVER:


    void AsyncObserver::notifyAsyncResultAvailable(AsyncProviderBase *ctx, Actor *actor) {
        if (actor && actor != Actor::currentActor()) {
            // Schedule a call on my Actor:
            actor->enqueueOther("AsyncObserver::asyncResultAvailable", this,
                                &AsyncObserver::asyncResultAvailable, retained(ctx));
        } else {
            // ... or call it synchronously:
            asyncResultAvailable(ctx);
        }
    }


#pragma mark - ASYNC PROVIDER BASE:


    AsyncProviderBase::AsyncProviderBase(Actor *actorOwningFn)
    :_fnState(new AsyncFnState(this, actorOwningFn))
    { }


    AsyncProviderBase::AsyncProviderBase(bool ready)
    :_ready(ready)
    { }


    AsyncProviderBase::~AsyncProviderBase() {
        if (!_ready)
            WarnError("AsyncProvider %p deleted without ever getting a value!", (void*)this);
    }


    void AsyncProviderBase::_start() {
        notifyAsyncResultAvailable(nullptr, _fnState->_owningActor);
    }


    void AsyncProviderBase::_wait() {
        _fnState->_awaiting->setObserver(this);
    }


    void AsyncProviderBase::setObserver(AsyncObserver *o, Actor *actor) {
        {
            unique_lock<decltype(_mutex)> _lock(_mutex);
            precondition(!_observer);
            // Presumably I wasn't ready when the caller decided to call `setObserver` on me;
            // but I might have become ready in between then and now, so check for that.
            if (!_ready) {
                _observer = o;
                _observerActor = actor ? actor : Actor::currentActor();
                return;
            }
        }
        // if I am ready, call the observer now:
        o->notifyAsyncResultAvailable(this, actor);
    }


    void AsyncProviderBase::_gotResult(std::unique_lock<std::mutex>& lock) {
        precondition(!_ready);
        _ready = true;
        auto observer = _observer;
        _observer = nullptr;
        auto observerActor = std::move(_observerActor);

        lock.unlock();

        if (observer)
            observer->notifyAsyncResultAvailable(this, observerActor);

        // If I am the result of an async fn, it must have finished, so forget its state:
        _fnState = nullptr;
    }


    void AsyncProviderBase::setException(std::exception_ptr x) {
        unique_lock<decltype(_mutex)> lock(_mutex);
        precondition(!_exception);
        _exception = x;
        _gotResult(lock);
    }


    void AsyncProviderBase::rethrowException() const {
        if (_exception)
            rethrow_exception(_exception);
    }


#pragma mark - ASYNC BASE:
    

    AsyncBase::AsyncBase(AsyncProviderBase *context, bool)
    :AsyncBase(context)
    {
        _provider->_start();
    }


    // Simple class that observes an AsyncProvider and can block until it's ready.
    class BlockingObserver : public AsyncObserver {
    public:
        BlockingObserver(AsyncProviderBase *provider)
        :_provider(provider)
        { }

        void wait() {
            unique_lock<decltype(_mutex)> lock(_mutex);
            _provider->setObserver(this);
            _cond.wait(lock, [&]{return _provider->ready();});
        }
    private:
        void asyncResultAvailable(Retained<AsyncProviderBase>) override {
            unique_lock<decltype(_mutex)> lock(_mutex);
            _cond.notify_one();
        }

        mutex              _mutex;
        condition_variable _cond;
        AsyncProviderBase* _provider;
    };


    void AsyncBase::blockUntilReady() {
        if (!ready()) {
            precondition(Actor::currentActor() == nullptr); // would deadlock if called by an Actor
            BlockingObserver obs(_provider);
            obs.wait();
        }
    }

}
