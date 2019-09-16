//
//  c4LocalReplicator.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/16/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Replicator.hh"
#include "LoopbackProvider.hh"

using namespace litecore::websocket;

namespace c4Internal {

    class C4LocalReplicator : public C4Replicator {
    public:
        C4LocalReplicator(C4Database* db,
                          const C4ReplicatorParameters &params,
                          C4Database* otherDB)
        :C4Replicator(db, params)
        ,_otherDatabase(otherDB)
        { }


        void start(bool synchronous =false) override {
            auto socket1 = new LoopbackWebSocket(Address(_database), Role::Client);
            auto socket2 = new LoopbackWebSocket(Address(_otherDatabase), Role::Server);
            _replicator = new Replicator(_database, socket1, *this,
                                         options().setNoDeltas());
            _otherReplicator = new Replicator(_otherDatabase, socket2, *this,
                                              Replicator::Options(kC4Passive, kC4Passive)
                                                .setNoIncomingConflicts().setNoDeltas());
            LoopbackWebSocket::bind(_replicator->webSocket(), _otherReplicator->webSocket());

            _selfRetainToo = this;
            _otherReplicator->start(synchronous);
            
            C4Replicator::start(synchronous);
        }


        virtual void replicatorStatusChanged(Replicator *repl,
                                             const Replicator::Status &newStatus) override
        {
            C4Replicator::replicatorStatusChanged(repl, newStatus);
            {
                lock_guard<mutex> lock(_mutex);
                if (repl == _otherReplicator && newStatus.level == kC4Stopped)
                    _selfRetainToo = nullptr; // balances retain in constructor
            }
        }

    private:
        Retained<C4Database> const  _otherDatabase;
        Retained<Replicator>        _otherReplicator;
        Retained<C4LocalReplicator> _selfRetainToo;
    };

}
