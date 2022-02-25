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


    AsyncProviderBase::AsyncProviderBase(bool ready)
    :_ready(ready)
    { }


    AsyncProviderBase::~AsyncProviderBase() {
        if (!_ready)
            WarnError("AsyncProvider %p deleted without ever getting a value!", (void*)this);
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
        // on entry, `_mutex` is locked by `lock`
        precondition(!_ready);
        _ready = true;
        auto observer = _observer;
        _observer = nullptr;
        auto observerActor = std::move(_observerActor);

        lock.unlock();

        if (observer)
            observer->notifyAsyncResultAvailable(this, observerActor);
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
    

    bool AsyncBase::canCallNow() const {
        return ready() && (_onActor == nullptr || _onActor == Actor::currentActor());
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
