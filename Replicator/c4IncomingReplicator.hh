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

    /** A passive replicator handling an incoming WebSocket connection, for P2P. */
    class C4IncomingReplicator : public C4Replicator {
    public:
        C4IncomingReplicator(C4Database* db NONNULL,
                             const C4ReplicatorParameters &params,
                             WebSocket *openSocket NONNULL)
        :C4Replicator(db, params)
        ,_openSocket(openSocket)
        { }

        virtual void start() override {
            LOCK(_mutex);
            Assert(_openSocket);
            _start(new Replicator(_database, _openSocket, *this, options()));
            _openSocket = nullptr;
        }

    private:
        WebSocket* _openSocket;
    };

}
