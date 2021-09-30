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


        virtual bool createReplicator() override {
            Assert(_openSocket);
            
            C4Error err;
            c4::ref<C4Database> dbCopy = c4db_openAgain(_database, &err);
            if(!dbCopy) {
                _status.error = err;
                return false;
            }
            c4db_setDatabaseTag(dbCopy, DatabaseTag_C4IncomingReplicator);
            
            _replicator = new Replicator(dbCopy, _openSocket, *this, _options);
            
            // Yes this line is disgusting, but the memory addresses that the logger logs
            // are not the _actual_ addresses of the object, but rather the pointer to
            // its Logging virtual table since inside of _logVerbose this is all that
            // is known.
            _logVerbose("C4IncomingRepl %p created Repl %p", (Logging *)this, (Logging *)_replicator.get());
            _openSocket = nullptr;
            return true;
        }


        virtual bool _unsuspend() override {
            // Restarting doesn't make sense; do nothing
            return true;
        }

    private:
        WebSocket* _openSocket;
    };

}
