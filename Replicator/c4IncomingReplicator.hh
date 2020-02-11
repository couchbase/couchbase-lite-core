//
//  C4IncomingReplicator.h
//  LiteCore
//
//  Created by Jens Alfke on 9/16/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Replicator.hh"
#include "c4Socket+Internal.hh"

namespace c4Internal {
    using namespace litecore::websocket;

    /** A passive replicator handling an incoming WebSocket connection, for P2P. */
    class C4IncomingReplicator : public C4Replicator {
    public:
        C4IncomingReplicator(C4Database* db NONNULL,
                             const C4ReplicatorParameters &params,
                             WebSocket *openSocket NONNULL)
        :C4Replicator(db, params)
        ,_openSocket(openSocket)
        { }

        
        virtual alloc_slice URL() const override {
            return _openSocket->url();
        }


        virtual void createReplicator() override {
            Assert(_openSocket);
            _replicator = new Replicator(_database, _openSocket, *this, _options);
            _openSocket = nullptr;
        }


        virtual void _unsuspend() override {
            // Restarting doesn't make sense; do nothing
        }

    private:
        WebSocket* _openSocket;
    };

}
