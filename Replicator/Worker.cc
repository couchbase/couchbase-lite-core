//
//  Worker.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Worker.hh"
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


    Worker::Worker(blip::Connection *connection,
                         Worker *parent,
                         Options options,
                         const char *namePrefix)
    :Actor( string(namePrefix) + connection->name() )
    ,Logging(SyncLog)
    ,_connection(connection)
    ,_parent(parent)
    ,_options(options)
    ,_status{(connection->state() >= Connection::kConnected) ? kC4Idle : kC4Connecting}
    { }


    Worker::Worker(Worker *parent,
                         const char *namePrefix)
    :Worker(parent->_connection, parent, parent->_options, namePrefix)
    {

    }


    Worker::~Worker() {
        if (_important)
            logStats();
    }


    void Worker::sendRequest(blip::MessageBuilder& builder, MessageProgressCallback callback) {
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


    blip::ErrorBuf Worker::c4ToBLIPError(C4Error err) {
        //FIX: Map common errors to more standard domains
        if (!err.code)
            return { };
        return {slice(kErrorDomainNames[err.domain]),
                err.code,
                alloc_slice(c4error_getMessage(err))};
    }


    C4Error Worker::blipToC4Error(const blip::Error &err) {
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


    void Worker::gotError(const MessageIn* msg) {
        auto err = msg->getError();
        logError("Got error response: %.*s %d '%.*s'",
                 SPLAT(err.domain), err.code, SPLAT(err.message));
        _status.error = blipToC4Error(err);
        _statusChanged = true;
    }

    void Worker::gotError(C4Error err) {
        alloc_slice message = c4error_getMessage(err);
        logError("Got LiteCore error: %.*s (%d/%d)", SPLAT(message), err.domain, err.code);
        _status.error = err;
        _statusChanged = true;
    }


#pragma mark - ACTIVITY / PROGRESS:


    void Worker::setProgress(C4Progress p) {
        addProgress(p - _status.progress);
    }


    void Worker::addProgress(C4Progress p) {
        if (p.completed || p.total) {
            _status.progressDelta += p;
            _status.progress += p;
            _statusChanged = true;
        }
    }



    Worker::ActivityLevel Worker::computeActivityLevel() const {
        if (eventCount() > 1 || _pendingResponseCount > 0)
            return kC4Busy;
        else
            return kC4Idle;
    }

    
    // Called after every event; updates busy status & detects when I'm done
    void Worker::afterEvent() {
        bool changed = _statusChanged;
        _statusChanged = false;
        if (changed && _important) {
            logVerbose("progress +%llu/+%llu -- now %llu / %llu",
                       _status.progressDelta.completed, _status.progressDelta.total,
                       _status.progress.completed, _status.progress.total);
        }

        auto newLevel = computeActivityLevel();
        if (newLevel != _status.level) {
            _status.level = newLevel;
            changed = true;
            if (_important)
                log("now %s", kC4ReplicatorActivityLevelNames[newLevel]);
        }
        if (changed)
            changedStatus();
        _status.progressDelta = {0, 0};
    }

    void Worker::changedStatus() {
        if (_parent)
            _parent->childChangedStatus(this, _status);
    }


} }
