//
//  c4Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Fleece.h"
#include "c4Internal.hh"
#include "c4Replicator.h"
#include "c4.hh"
#include "LibWSProvider.hh"
#include "Replicator.hh"
#include <atomic>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;
using namespace litecore::repl;
using namespace litecore::websocket;


namespace litecore {
    static inline std::string asstring(C4String s) {
        return std::string((char*)s.buf, s.size);
    }
}


struct C4Replicator : public RefCounted, Replicator::Delegate {

    C4Replicator(C4Database* db,
                 C4Address c4addr,
                 C4ReplicationOptions c4opts)
    {
        static websocket::Provider *sWSProvider = new websocket::LibWSProvider();
        websocket::Address address(asstring(c4addr.scheme),
                                   asstring(c4addr.hostname),
                                   c4addr.port,
                                   asstring(c4addr.path));
        Replicator::Options options{ c4opts.push, c4opts.pull };
        _onStateChanged = c4opts.onStateChanged;
        _replicator = new Replicator(db, *sWSProvider, address, *this, options);
    }

    
    virtual void replicatorActivityChanged(Replicator*, Replicator::ActivityLevel level) override {
        _state = level;
        notify();
    }

    virtual void replicatorCloseStatusChanged(Replicator*,
                                              const Replicator::CloseStatus &status) override
    {
        static const C4ErrorDomain kDomainForReason[] = {WebSocketDomain, POSIXDomain, DNSDomain};
        if (status.reason == kWebSocketClose && (status.code == kCodeNormal
                                                 || status.code == kCodeGoingAway)) {
            _error = {};
        } else {
            _error = {kDomainForReason[status.reason], status.code};
        }
        notify();
    }

    void notify() {
        if (_onStateChanged)
            _onStateChanged(this, _state, _error);
    }

    C4ReplicationState state() const  {return _state;}

private:
    Retained<Replicator> _replicator;
    atomic<C4ReplicationState> _state {kConnecting};
    C4Error _error;
    C4ReplicatorStateChangedCallback _onStateChanged;
};


C4Replicator* c4repl_new(C4Database* db,
                         C4Address c4addr,
                         C4ReplicationOptions c4opts,
                         C4Error *err) C4API
{
    try {
        return retain(new C4Replicator(db, c4addr, c4opts));
    } catch (const std::exception &x) {
        WarnError("Exception caught in c4repl_new");    //FIX: Set *err
        return nullptr;
    }
}


void c4repl_free(C4Replicator* repl) C4API {
    release(repl);
}


C4ReplicationState c4repl_getState(C4Replicator *repl) C4API {
    return repl->state();
}
