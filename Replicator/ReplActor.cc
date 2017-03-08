//
//  ReplActor.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ReplActor.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "PlatformCompat.hh"

#if defined(__clang__) && !defined(__ANDROID__)
#include <cxxabi.h>
#endif

using namespace std;
using namespace litecore::blip;

namespace litecore { namespace repl {


    static LogDomain SyncLog("Sync");


    ReplActor::ReplActor(blip::Connection *connection,
                         Options options,
                         const char *namePrefix)
    :Actor( string(namePrefix) + connection->name() )
    ,Logging(SyncLog)
    ,_connection(connection)
    ,_options(options)
    { }


    void ReplActor::sendRequest(blip::MessageBuilder& builder,
                                std::function<void(MessageIn*)> callback) {
        auto r = _connection->sendRequest(builder);
        if (callback) {
            ++_pendingResponseCount;
            onReady(r, [this, callback](Retained<MessageIn> response) {
                --_pendingResponseCount;
                callback(response);
            });
        } else {
            if (!builder.noreply)
                warn("Ignoring the response to a BLIP message!");
        }
    }


    void ReplActor::gotError(const MessageIn* msg) {
        // TODO
        logError("Got error response: %.*s %d",
                             SPLAT(msg->errorDomain()), msg->errorCode());
    }

    void ReplActor::gotError(C4Error err) {
        // TODO
        alloc_slice message = c4error_getMessage(err);
        logError("Got LiteCore error: %.*s (%d/%d)", SPLAT(message), err.domain, err.code);
    }


    ReplActor::ActivityLevel ReplActor::computeActivityLevel() const {
        if (eventCount() > 1 || _pendingResponseCount > 0)
            return kBusy;
        else
            return kIdle;
    }


    // Called after every event; updates busy status & detects when I'm done
    void ReplActor::afterEvent() {
        auto newLevel = computeActivityLevel();
        if (newLevel != _activityLevel) {
            _activityLevel = newLevel;
            const char *kLevelNames[] = {"stopped!", "connecting", "idle", "busy"};
            log("now %s", kLevelNames[newLevel]);
            activityLevelChanged(newLevel);
        }
    }


} }
