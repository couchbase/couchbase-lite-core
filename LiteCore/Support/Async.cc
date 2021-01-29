//
// Async.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
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

#include "Async.hh"
#include "Actor.hh"

namespace litecore { namespace actor {


    bool AsyncState::_asyncCall(const AsyncBase &a, int lineNo) {
        _calling = a._context;
        _continueAt = lineNo;
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

    void AsyncContext::start() {
        _waitingSelf = this;
        if (_actor && _actor != Actor::currentActor())
            _actor->wakeAsyncContext(this);     // Start on my Actor's queue
        else
            next();
    }

    void AsyncContext::_wait() {
        _waitingActor = Actor::currentActor();  // retain my actor while I'm waiting
        _calling->setObserver(this);
    }

    void AsyncContext::wakeUp(AsyncContext *async) {
        assert(async == _calling);
        if (_waitingActor) {
            fleece::Retained<Actor> waitingActor = std::move(_waitingActor);
            waitingActor->wakeAsyncContext(this);       // queues the next() call on its Mailbox
        } else {
            next();
        }
    }

    void AsyncContext::_gotResult() {
        _ready = true;
        auto observer = _observer;
        _observer = nullptr;
        if (observer)
            observer->wakeUp(this);
        _waitingSelf = nullptr;
    }

} }
