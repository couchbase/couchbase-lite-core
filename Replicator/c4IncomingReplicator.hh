//
//  C4IncomingReplicator.h
//  LiteCore
//
//  Created by Jens Alfke on 9/16/19.
//  Copyright 2019-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once
#include "c4Private.h"
#include "c4ReplicatorImpl.hh"

namespace litecore {
    using namespace litecore::websocket;

    /** A passive replicator handling an incoming WebSocket connection, for P2P. */
    class C4IncomingReplicator final : public C4ReplicatorImpl {
      public:
        C4IncomingReplicator(C4Database* db NONNULL, const C4ReplicatorParameters& params,
                             WebSocket* openSocket NONNULL, slice logPrefix)
            : C4ReplicatorImpl(db, params), _openSocket(openSocket) {
            std::string logName = "C4IncomingRepl";
            if ( !logPrefix.empty() ) logName = logPrefix.asString() + "/" + logName;
            setLoggingName(logName);
        }

        alloc_slice URL() const noexcept override { return _openSocket->url(); }

        void createReplicator() override {
            Assert(_openSocket);
            _replicator = new Replicator(makeDBAccess(_database, DatabaseTag_C4IncomingReplicator), _openSocket, *this,
                                         _options);

            // Yes this line is disgusting, but the memory addresses that the logger logs
            // are not the _actual_ addresses of the object, but rather the pointer to
            // its Logging virtual table since inside of _logVerbose this is all that
            // is known.
            _logVerbose("C4IncomingRepl %p created Repl %p", (Logging*)this, (Logging*)_replicator.get());
            _openSocket = nullptr;
        }

        bool _unsuspend() noexcept override {
            // Restarting doesn't make sense; do nothing
            return true;
        }

      private:
        WebSocket* _openSocket;
    };

}  // namespace litecore
