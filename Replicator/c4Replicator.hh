//
//  c4Replicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 5/21/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once

#include "FleeceCpp.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Replicator.h"
#include "Replicator.hh"
#include "c4Socket+Internal.hh"
#include "LoopbackProvider.hh"
#include "Error.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


struct C4Replicator : public RefCounted, Replicator::Delegate {

    // Constructor for replication with remote database
    C4Replicator(C4Database* db,
                 const C4Address &remoteAddress,
                 C4String remoteDatabaseName,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4Slice properties,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :C4Replicator(new Replicator(db, C4Provider::instance(),
                                 addressFrom(remoteAddress, remoteDatabaseName),
                                 *this, { push, pull, properties }),
                  nullptr,
                  onStateChanged, callbackContext)
    {
        _replicator->start();
    }

    // Constructor for replication with local database
    C4Replicator(C4Database* db,
                 C4Database* otherDB,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4Slice properties,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :C4Replicator(new Replicator(db, loopbackProvider(),
                                 addressFrom(otherDB),
                                 *this, { push, pull, properties }),
                  new Replicator(otherDB,
                                 loopbackProvider().createWebSocket(addressFrom(db)),
                                 *this, { kC4Passive, kC4Passive }),
                  onStateChanged, callbackContext)
    {
        loopbackProvider().bind(_replicator->webSocket(), _otherReplicator->webSocket());
        _otherLevel = _otherReplicator->status().level;
        _otherReplicator->start();
        _replicator->start();
    }

    // Constructor for already-open socket
    C4Replicator(C4Database* db,
                 C4Socket *openSocket,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4Slice properties,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :C4Replicator(new Replicator(db, WebSocketFrom(openSocket), *this, {push, pull, properties}),
                  nullptr,
                  onStateChanged, callbackContext)
    {
        _replicator->start();
    }

    const AllocedDict& responseHeaders() {
        lock_guard<mutex> lock(_mutex);
        return _responseHeaders;
    }

    C4ReplicatorStatus status() {
        lock_guard<mutex> lock(_mutex);
        return _status;
    }

    void stop() {
        _replicator->stop();
    }

    void detach() {
        lock_guard<mutex> lock(_mutex);
        _onStateChanged = nullptr;
    }

private:
    // base constructor
    C4Replicator(Replicator *replicator,
                 Replicator *otherReplicator,
                 C4ReplicatorStatusChangedCallback onStateChanged,
                 void *callbackContext)
    :_replicator(replicator)
    ,_otherReplicator(otherReplicator)
    ,_onStateChanged(onStateChanged)
    ,_callbackContext(callbackContext)
    ,_status(_replicator->status())
    ,_selfRetain(this) // keep myself alive till replicator closes
    { }

    virtual ~C4Replicator() =default;

    static LoopbackProvider& loopbackProvider() {
        static LoopbackProvider sProvider;
        return sProvider;
    }

    virtual void replicatorGotHTTPResponse(Replicator *repl, int status,
                                           const AllocedDict &headers) override
    {
        lock_guard<mutex> lock(_mutex);
        if (repl == _replicator) {
            Assert(!_responseHeaders);
            _responseHeaders = headers;
        }
    }

    virtual void replicatorStatusChanged(Replicator *repl,
                                         const Replicator::Status &newStatus) override
    {
        bool done;
        {
            lock_guard<mutex> lock(_mutex);
            if (repl == _replicator)
                _status = newStatus;
            else if (repl == _otherReplicator)
                _otherLevel = newStatus.level;
            done = (_status.level == kC4Stopped && _otherLevel == kC4Stopped);
        }

        if (repl == _replicator)
            notify();
        if (done)
            _selfRetain = nullptr; // balances retain in constructor
    }

    void notify() {
        C4ReplicatorStatusChangedCallback onStateChanged;
        {
            lock_guard<mutex> lock(_mutex);
            onStateChanged = _onStateChanged;
        }
        if (onStateChanged)
            onStateChanged(this, _status, _callbackContext);
    }

    mutex _mutex;
    Retained<Replicator> const _replicator;
    Retained<Replicator> const _otherReplicator;
    C4ReplicatorStatusChangedCallback _onStateChanged;
    void* const _callbackContext;
    AllocedDict _responseHeaders;
    C4ReplicatorStatus _status;
    C4ReplicatorActivityLevel _otherLevel {kC4Stopped};
    Retained<C4Replicator> _selfRetain;
};
