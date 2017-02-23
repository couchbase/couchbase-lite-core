//
//  ReplActor.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ReplActor.hh"
#include "Logging.hh"
#include "PlatformCompat.hh"

#if defined(__clang__) && !defined(__ANDROID__)
#include <cxxabi.h>
#endif

using namespace litecore::blip;

namespace litecore { namespace repl {


    LogDomain SyncLog("Sync");


    std::string ReplActor::loggingIdentifier() const {
        return Logging::loggingIdentifier();// name();
    }



    void ReplActor::gotError(const MessageIn* msg) {
        // TODO
        log<LogLevel::Error>("Got error response: %.*s %d",
                             SPLAT(msg->errorDomain()), msg->errorCode());
    }

    void ReplActor::gotError(C4Error err) {
        // TODO
        alloc_slice message = c4error_getMessage(err);
        log<LogLevel::Error>("Got error response: %.*s (%d/%d)", SPLAT(message), err.domain, err.code);
    }


    void ReplActor::setBusy(bool busy) {
        if (busy != _busy) {
            _busy = busy;
            logDebug("Now %s", (busy ? "busy" : "idle"));
        }
    }


} }
