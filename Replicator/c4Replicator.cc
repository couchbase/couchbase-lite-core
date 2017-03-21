//
//  c4Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "FleeceCpp.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Replicator.h"
#include "c4Socket+Internal.hh"
#include "Replicator.hh"
#include "StringUtil.hh"
#include <atomic>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


struct C4Replicator : public RefCounted, Replicator::Delegate {

    C4Replicator(C4Database* db,
                 C4Address remoteAddress,
                 C4String remoteDatabaseName,
                 C4ReplicatorMode push,
                 C4ReplicatorMode pull,
                 C4ReplicatorStateChangedCallback onStateChanged,
                 void *callbackContext)
    {
        websocket::Address address(asstring(remoteAddress.scheme),
                                   asstring(remoteAddress.hostname),
                                   remoteAddress.port,
                                   format("/%.*s/_blipsync", SPLAT(remoteDatabaseName)));
        _onStateChanged = onStateChanged;
        _callbackContext = callbackContext;
        _replicator = new Replicator(db, DefaultProvider(), address, *this, { push, pull });
        _state = {_replicator->activityLevel(), {}};
        retain(this); // keep myself alive till replicator closes
    }

    C4ReplicatorState state() const     {return _state;}

    void stop()                         {_replicator->stop();}

    void detach()                       {_onStateChanged = nullptr;}

private:

    virtual void replicatorActivityChanged(Replicator*, Replicator::ActivityLevel level) override {
        if (setActivityLevel(level)) {
            notify();
            if (level == kC4Stopped)
                release(this); // balances retain in constructor
        }
    }

    virtual void replicatorConnectionClosed(Replicator*,
                                            const Replicator::CloseStatus &status) override
    {
        // The replicator may do a bit of work after the connection closes,
        // so don't assume it's stopped yet
        static const C4ErrorDomain kDomainForReason[] = {WebSocketDomain, POSIXDomain, DNSDomain};

        if (status.reason != kWebSocketClose || (status.code != kCodeNormal
                                                 && status.code != kCodeGoingAway)) {
            C4ReplicatorState state = _state;
            state.error = c4error_make(kDomainForReason[status.reason], status.code, status.message);
            _state = state;
            notify();
        }
    }

    bool setActivityLevel(C4ReplicatorActivityLevel level) {
        C4ReplicatorState state = _state;
        if (state.level == level)
            return false;
        state.level = level;
        _state = state;
        return true;
    }

    
    void notify() {
        C4ReplicatorStateChangedCallback on = _onStateChanged;
        if (on)
            on(this, _state, _callbackContext);
    }

    Retained<Replicator> _replicator;
    atomic<C4ReplicatorState> _state;
    atomic<C4ReplicatorStateChangedCallback> _onStateChanged;
    void *_callbackContext;
};


C4Replicator* c4repl_new(C4Database* db,
                         C4Address c4addr,
                         C4String remoteDatabaseName,
                         C4ReplicatorMode push,
                         C4ReplicatorMode pull,
                         C4ReplicatorStateChangedCallback onStateChanged,
                         void *callbackContext,
                         C4Error *err) C4API
{
    try {
        if (push == kC4Disabled && pull == kC4Disabled) {
            if (err)
                *err = c4error_make(LiteCoreDomain, kC4ErrorInvalidParameter,
                                    C4STR("Either push or pull must be enabled"));
            return nullptr;
        }
        return retain(new C4Replicator(db, c4addr, remoteDatabaseName,
                                       push, pull, onStateChanged, callbackContext));
    } catch (const std::exception &x) {
        if (err)
            *err = c4error_make(LiteCoreDomain, kC4ErrorUnexpectedError, slice(x.what()));
        // TODO: Return a better error
        return nullptr;
    }
}


void c4repl_stop(C4Replicator* repl) C4API {
    repl->stop();
}


void c4repl_free(C4Replicator* repl) C4API {
    if (!repl)
        return;
    repl->stop();
    repl->detach();
    release(repl);
}


C4ReplicatorState c4repl_getState(C4Replicator *repl) C4API {
    return repl->state();
}
