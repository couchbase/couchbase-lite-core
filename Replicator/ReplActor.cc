//
//  ReplActor.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ReplActor.hh"
#include "Replicator.hh"
#include "ReplicatorTypes.hh"
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
                         Replicator *replicator,
                         Options options,
                         const char *namePrefix)
    :Actor( string(namePrefix) + connection->name() )
    ,Logging(SyncLog)
    ,_connection(connection)
    ,_replicator(replicator)
    ,_options(options)
    {
        _status.level = (connection->state() >= Connection::kConnected) ? kC4Idle : kC4Connecting;
    }


    void ReplActor::sendRequest(blip::MessageBuilder& builder, MessageProgressCallback callback) {
        if (callback) {
            ++_pendingResponseCount;
            builder.onProgress = asynchronize([=](MessageProgress progress) {
                if (progress.state == MessageProgress::kComplete)
                    --_pendingResponseCount;
                callback(progress);
            });
        } else {
            if (!builder.noreply)
                warn("Ignoring the response to a BLIP message!");
        }
        assert(_connection);
        _connection->sendRequest(builder);
    }


#pragma mark - ERRORS:


    static const char* const kErrorDomainNames[] = {
        nullptr, "LiteCore", "POSIX", nullptr, "SQLite", "Fleece", "DNS", "WebSocket"};


    blip::ErrorBuf ReplActor::c4ToBLIPError(C4Error err) {
        if (!err.code)
            return { };
        return {slice(kErrorDomainNames[err.domain]),
                err.code,
                alloc_slice(c4error_getMessage(err))};
    }


    C4Error ReplActor::blipToC4Error(const blip::Error &err) {
        if (!err.domain)
            return { };
        C4ErrorDomain domain = LiteCoreDomain;
        int code = kC4ErrorRemoteError;
        string domainStr = err.domain.asString();
        const char* domainCStr = domainStr.c_str();
        for (uint32_t d = 0; d <= WebSocketDomain; d++) {
            if (kErrorDomainNames[d] && strcmp(domainCStr, kErrorDomainNames[d]) == 0) {
                domain = (C4ErrorDomain)d;
                code = err.code;
                break;
            }
        }
        return c4error_make(domain, code, err.message);
    }


    void ReplActor::gotError(const MessageIn* msg) {
        auto err = msg->getError();
        logError("Got error response: %.*s %d '%.*s'",
                 SPLAT(err.domain), err.code, SPLAT(err.message));
        _status.error = blipToC4Error(err);
        _statusChanged = true;
    }

    void ReplActor::gotError(C4Error err) {
        alloc_slice message = c4error_getMessage(err);
        logError("Got LiteCore error: %.*s (%d/%d)", SPLAT(message), err.domain, err.code);
        _status.error = err;
        _statusChanged = true;
    }


#pragma mark - ACTIVITY / PROGRESS:


    void ReplActor::setProgress(C4Progress p) {
        if (p != _status.progress) {
            _status.progress = p;
            _statusChanged = true;
        }
    }


    void ReplActor::addProgress(C4Progress p) {
        setProgress(p + _status.progress);
    }



    ReplActor::ActivityLevel ReplActor::computeActivityLevel() const {
        if (eventCount() > 1 || _pendingResponseCount > 0)
            return kC4Busy;
        else
            return kC4Idle;
    }

    
    // Called after every event; updates busy status & detects when I'm done
    void ReplActor::afterEvent() {
        bool changed = _statusChanged;
        _statusChanged = false;
        auto newLevel = computeActivityLevel();
        if (newLevel != _status.level) {
            _status.level = newLevel;
            log("now %s", kC4ReplicatorActivityLevelNames[newLevel]);
            changed = true;
        }
        if (changed)
            changedActivityLevel();
    }

    void ReplActor::changedActivityLevel() {
        if (_replicator && _replicator != this)
            _replicator->taskChangedStatus(this, _status);
    }


} }
