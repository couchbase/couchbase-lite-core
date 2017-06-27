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

    static Replicator::Options mkopts(const C4ReplicatorParameters &params) {
        Replicator::Options opts(params.push, params.pull, params.optionsDictFleece);
        opts.pullValidator = params.validationFunc;
        opts.pullValidatorContext = params.callbackContext;
        return opts;
    }

    // Constructor for replication with remote database
    C4Replicator(C4Database* db,
                 const C4Address &remoteAddress,
                 C4String remoteDatabaseName,
                 const C4ReplicatorParameters &params)
    :C4Replicator(new Replicator(db, C4Provider::instance(),
                                 addressFrom(remoteAddress, remoteDatabaseName), *this,
                                 mkopts(params)),
                  nullptr,
                  params)
    {
        _replicator->start();
    }

    // Constructor for replication with local database
    C4Replicator(C4Database* db,
                 C4Database* otherDB,
                 const C4ReplicatorParameters &params)
    :C4Replicator(new Replicator(db, loopbackProvider(),
                                 addressFrom(otherDB), *this, mkopts(params)),
                  new Replicator(otherDB,
                                 loopbackProvider().createWebSocket(addressFrom(db)),
                                 *this, { kC4Passive, kC4Passive }),
                  params)
    {
        loopbackProvider().bind(_replicator->webSocket(), _otherReplicator->webSocket());
        _otherLevel = _otherReplicator->status().level;
        _otherReplicator->start();
        _replicator->start();
    }

    // Constructor for already-open socket
    C4Replicator(C4Database* db,
                 C4Socket *openSocket,
                 const C4ReplicatorParameters &params)
    :C4Replicator(new Replicator(db, WebSocketFrom(openSocket), *this, mkopts(params)),
                  nullptr,
                  params)
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
        _params.onStatusChanged = nullptr;
        _params.onDocumentError = nullptr;
    }

private:
    // base constructor
    C4Replicator(Replicator *replicator,
                 Replicator *otherReplicator,
                 const C4ReplicatorParameters &params)
    :_replicator(replicator)
    ,_otherReplicator(otherReplicator)
    ,_params(params)
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
            notifyStateChanged();
        if (done)
            _selfRetain = nullptr; // balances retain in constructor
    }

    virtual void replicatorDocumentError(Replicator *repl,
                                         bool pushing,
                                         slice docID,
                                         C4Error error,
                                         bool transient) override
    {
        if (repl != _replicator)
            return;
        C4ReplicatorDocumentErrorCallback onDocError;
        {
            lock_guard<mutex> lock(_mutex);
            onDocError = _params.onDocumentError;
        }
        if (onDocError)
            onDocError(this, pushing, {docID.buf, docID.size}, error, transient,
                       _params.callbackContext);
    }

    void notifyStateChanged() {
        C4ReplicatorStatusChangedCallback onStatusChanged;
        {
            lock_guard<mutex> lock(_mutex);
            onStatusChanged = _params.onStatusChanged;
        }
        if (onStatusChanged)
            onStatusChanged(this, _status, _params.callbackContext);
    }

    mutex _mutex;
    Retained<Replicator> const _replicator;
    Retained<Replicator> const _otherReplicator;
    C4ReplicatorParameters _params;
    AllocedDict _responseHeaders;
    C4ReplicatorStatus _status;
    C4ReplicatorActivityLevel _otherLevel {kC4Stopped};
    Retained<C4Replicator> _selfRetain;
};
