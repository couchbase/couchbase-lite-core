//
//  ReplActor.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ReplActor.hh"
#include "Logging.hh"

using namespace litecore::blip;

namespace litecore { namespace repl {


    LogDomain SyncLog("Sync");

    
    void ReplActor::setConnection(blip::Connection *connection) {
        assert(!_connection);
        assert(connection);
        _connection = connection;
    }


    void ReplActor::gotError(const MessageIn* msg) {
        // TODO
        LogToAt(SyncLog, Error, "Got error response: %.*s %d",
                SPLAT(msg->errorDomain()), msg->errorCode());
    }

    void ReplActor::gotError(C4Error err) {
        // TODO
        LogToAt(SyncLog, Error, "Got error response: %d/%d", err.domain, err.code);
    }

    
} }
