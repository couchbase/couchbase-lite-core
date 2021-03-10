//
//  C4IncomingReplicator.h
//  LiteCore
//
//  Created by Jens Alfke on 9/16/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4ReplicatorImpl.hh"
#include "c4Socket+Internal.hh"

namespace c4Internal {
    using namespace litecore::websocket;

    /** A passive replicator handling an incoming WebSocket connection, for P2P. */
    class C4IncomingReplicator : public C4ReplicatorImpl {
    public:
        C4IncomingReplicator(C4Database* db NONNULL,
                             const C4ReplicatorParameters &params,
                             WebSocket *openSocket NONNULL)
        :C4ReplicatorImpl(db, params)
        ,_openSocket(openSocket)
        { }

        
        virtual alloc_slice URL() const noexcept override {
            return _openSocket->url();
        }


        virtual void createReplicator() override {
            Assert(_openSocket);
            
            _replicator = new Replicator(_database->openAgain().get(),
                                         _openSocket, *this, _options);
            
            // Yes this line is disgusting, but the memory addresses that the logger logs
            // are not the _actual_ addresses of the object, but rather the pointer to
            // its Logging virtual table since inside of _logVerbose this is all that
            // is known.
            _logVerbose("C4IncomingRepl %p created Repl %p", (Logging *)this, (Logging *)_replicator.get());
            _openSocket = nullptr;
        }


        virtual bool _unsuspend() noexcept override {
            // Restarting doesn't make sense; do nothing
            return true;
        }

    private:
        WebSocket* _openSocket;
    };

}
