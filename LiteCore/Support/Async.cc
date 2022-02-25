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
#include "c4Error.h"
#include "c4Internal.hh"
#include "Logging.hh"
#include "betterassert.hh"

namespace litecore::actor {
    using namespace std;

#pragma mark - ASYNC OBSERVER:


#pragma mark - ASYNC PROVIDER BASE:


    AsyncProviderBase::AsyncProviderBase(bool ready)
    :_ready(ready)
    { }


    AsyncProviderBase::~AsyncProviderBase() {
        if (!_ready)
            WarnError("AsyncProvider %p deleted without ever getting a value!", (void*)this);
    }


    void AsyncProviderBase::setObserver(Actor *actor, Observer observer) {
        {
            unique_lock<decltype(_mutex)> _lock(_mutex);
            precondition(!_observer);
            // Presumably I wasn't ready when the caller decided to call `setObserver` on me;
            // but I might have become ready in between then and now, so check for that.
            if (!_ready) {
                _observer = move(observer);
                _observerActor = actor ? actor : Actor::currentActor();
                return;
            }
        }
        // if I am ready, call the observer now:
        notifyObserver(observer, actor);
    }


    void AsyncProviderBase::notifyObserver(Observer &observer, Actor *actor) {
        if (actor && actor != Actor::currentActor()) {
            // Schedule a call on my Actor:
            actor->asCurrentActor([observer, provider=fleece::retained(this)] {
                observer(*provider);
            });
        } else {
            // ... or call it synchronously:
            observer(*this);
        }
    }


    void AsyncProviderBase::_gotResult(std::unique_lock<std::mutex>& lock) {
        // on entry, `_mutex` is locked by `lock`
        precondition(!_ready);
        _ready = true;
        auto observer = move(_observer);
        _observer = nullptr;
        auto observerActor = move(_observerActor);
        _observerActor = nullptr;

        lock.unlock();

        if (observer)
            notifyObserver(observer, observerActor);
    }


    C4Error AsyncProviderBase::c4Error() const {
        return _error ? C4Error::fromException(*_error) : C4Error{};
    }


    void AsyncProviderBase::setError(const C4Error &c4err) {
        precondition(c4err.code != 0);
        unique_lock<decltype(_mutex)> lock(_mutex);
        precondition(!_error);
        _error = make_unique<litecore::error>(c4err);
        _gotResult(lock);
    }


    void AsyncProviderBase::setError(const std::exception &x) {
        auto e = litecore::error::convertException(x);
        unique_lock<decltype(_mutex)> lock(_mutex);
        precondition(!_error);
        _error = make_unique<litecore::error>(move(e));
        _gotResult(lock);
    }


    void AsyncProviderBase::throwIfError() const {
        if (_error)
            throw *_error;
    }


#pragma mark - ASYNC BASE:
    

    bool AsyncBase::canCallNow() const {
        return ready() && (_onActor == nullptr || _onActor == Actor::currentActor());
    }


    C4Error AsyncBase::c4Error() const {
        return _provider->c4Error();
    }


    void AsyncBase::blockUntilReady() {
        if (!ready()) {
            precondition(Actor::currentActor() == nullptr); // would deadlock if called by an Actor
            mutex _mutex;
            condition_variable _cond;

            _provider->setObserver(nullptr, [&](AsyncProviderBase &provider) {
                unique_lock<decltype(_mutex)> lock(_mutex);
                _cond.notify_one();
            });

            unique_lock<decltype(_mutex)> lock(_mutex);
            _cond.wait(lock, [&]{return ready();});
        }
    }

}
