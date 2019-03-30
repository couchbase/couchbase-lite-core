//
// Worker.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Worker.hh"
#include "Replicator.hh"
#include "ReplicatorTypes.hh"
#include "c4Private.h"
#include "Logging.hh"
#include "StringUtil.hh"
#include "PlatformCompat.hh"
#include "BLIP.hh"
#include <sstream>

#if defined(__clang__) && !defined(__ANDROID__)
#include <cxxabi.h>
#endif

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore {
    LogDomain SyncLog("Sync");
}

namespace litecore { namespace repl {

    LogDomain SyncBusyLog("SyncBusy", LogLevel::Warning);


    static void writeRedacted(Dict dict, stringstream &s) {
        s << "{";
        int n = 0;
        for (Dict::iterator i(dict); i; ++i) {
            if (n++ > 0)
                s << ", ";
            slice key = i.keyString();
            s << key << ":";
            if (key == slice(C4STR(kC4ReplicatorAuthPassword))) {
                s << "\"********\"";
            } else if (i.value().asDict()) {
                writeRedacted(i.value().asDict(), s);
            } else {
                alloc_slice json( i.value().toJSON5() );
                s << json;
            }
        }
        s << "}";
    }


    Options::operator string() const {
        static const char* kModeNames[] = {"disabled", "passive", "one-shot", "continuous"};
        stringstream s;
        if (push != kC4Disabled)
            s << "Push=" << kModeNames[push] << ", ";
        if (pull != kC4Disabled)
            s << "Pull=" << kModeNames[pull] << ", ";
        s << "Options={";
        writeRedacted(properties, s);
        s << "}";
        return s.str();
    }


    Worker::Worker(blip::Connection *connection,
                   Worker *parent,
                   const Options &options,
                   std::shared_ptr<DBAccess> dbAccess,
                   const char *namePrefix)
    :Actor(string(namePrefix) + connection->name(),
           (parent ? parent->mailboxForChildren() : nullptr))
    ,Logging(SyncLog)
    ,_connection(connection)
    ,_parent(parent)
    ,_options(options)
    ,_db(dbAccess)
    ,_progressNotificationLevel(options.progressLevel())
    ,_status{(connection->state() >= Connection::kConnected) ? kC4Idle : kC4Connecting}
    ,_loggingID(connection->name())
    { }


    Worker::Worker(Worker *parent, const char *namePrefix)
    :Worker(parent->_connection, parent, parent->_options, parent->_db, namePrefix)
    { }


    Worker::~Worker() {
        if (_important)
            logStats();
        logDebug("destructing (%p); actorName='%s'", this, actorName().c_str());
    }


    string Worker::loggingClassName() const  {
        string className = Logging::loggingClassName();
        if (_options.pull < kC4OneShot)
            toLowercase(className);
        return className;
    }


    void Worker::sendRequest(blip::MessageBuilder& builder, MessageProgressCallback callback) {
        if (callback) {
            increment(_pendingResponseCount);
            builder.onProgress = asynchronize([=](MessageProgress progress) {
                if (progress.state >= MessageProgress::kComplete)
                    decrement(_pendingResponseCount);
                callback(progress);
            });
        } else {
            if (!builder.noreply)
                warn("Ignoring the response to a BLIP message!");
        }
        DebugAssert(_connection);
        _connection->sendRequest(builder);
    }


#pragma mark - ERRORS:


    blip::ErrorBuf Worker::c4ToBLIPError(C4Error err) {
        //FIX: Map common errors to more standard domains
        if (!err.code)
            return { };
        slice blipDomain;
        if (err.domain == WebSocketDomain && err.code < 1000)
            blipDomain = "HTTP"_sl;
        else
            blipDomain = slice(error::nameOfDomain((error::Domain)err.domain));
        return {blipDomain, err.code, alloc_slice(c4error_getMessage(err))};
    }


    C4Error Worker::blipToC4Error(const blip::Error &err) {
        if (!err.domain || err.code == 0)
            return { };
        C4ErrorDomain domain = LiteCoreDomain;
        int code = 0;
        if (err.domain == "HTTP"_sl) {
            domain = WebSocketDomain;
            code = err.code;
        } else {
            for (error::Domain d = error::LiteCore; d < error::NumDomainsPlus1; d = (error::Domain)(d+1)) {
                if (err.domain == slice(error::nameOfDomain(d))) {
                    domain = (C4ErrorDomain)d;
                    code = err.code;
                    break;
                }
            }
        }
        if (code == 0) {
            LogToAt(SyncLog, Warning, "Received unknown error {'%.*s' %d \"%.*s\"} from server",
                    SPLAT(err.domain), err.code, SPLAT(err.message));
            code = kC4ErrorRemoteError;
        }
        return c4error_make(domain, code, err.message);
    }


    void Worker::gotError(const MessageIn* msg) {
        auto err = msg->getError();
        logError("Got error response: %.*s %d '%.*s'",
                 SPLAT(err.domain), err.code, SPLAT(err.message));
        onError(blipToC4Error(err));
    }

    void Worker::gotError(C4Error err) {
        alloc_slice message = c4error_getDescription(err);
        logError("Got LiteCore error: %.*s", SPLAT(message));
        onError(err);
    }

    void Worker::caughtException(const std::exception &x) {
        logError("Threw C++ exception: %s", x.what());
        onError(c4error_make(LiteCoreDomain, kC4ErrorUnexpectedError, slice(x.what())));
    }

    void Worker::onError(C4Error err) {
        _status.error = err;
        _statusChanged = true;
    }


    Replicator* Worker::replicator() const {
        const Worker *root = this;
        while (root->_parent)
            root = root->_parent;
        Assert(root);
        return (Replicator*)root;
    }


    void Worker::finishedDocumentWithError(ReplicatedRev *rev, C4Error error, bool transient) {
        rev->error = error;
        rev->errorIsTransient = transient;
        finishedDocument(rev);
    }


    void Worker::finishedDocument(ReplicatedRev *rev) {
        if (rev->error.code == 0)
            addProgress({0, 0, 1});
        if (rev->error.code || rev->isWarning || _progressNotificationLevel >= 1)
            replicator()->endedDocument(rev);
    }


#pragma mark - ACTIVITY / PROGRESS:


    void Worker::setProgress(C4Progress p) {
        addProgress(p - _status.progress);
    }


    void Worker::addProgress(C4Progress p) {
        if (p.unitsCompleted || p.unitsTotal || p.documentCount) {
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
            logVerbose("progress +%" PRIu64 "/+%llu, %" PRIu64 " docs -- now %" PRIu64 " / %" PRIu64 ", %" PRIu64 " docs",
                       _status.progressDelta.unitsCompleted, _status.progressDelta.unitsTotal,
                       _status.progressDelta.documentCount,
                       _status.progress.unitsCompleted, _status.progress.unitsTotal,
                       _status.progress.documentCount);
        }

        auto newLevel = computeActivityLevel();
        if (newLevel != _status.level) {
            _status.level = newLevel;
            changed = true;
            if (_important) {
                auto name = kC4ReplicatorActivityLevelNames[newLevel];
                if (_important > 1)
                    logInfo("now %-s", name);
                else
                    logVerbose("now %-s", name);
            }
        }
        if (changed)
            changedStatus();
        _status.progressDelta = {0, 0};
    }

    void Worker::changedStatus() {
        if (_parent)
            _parent->childChangedStatus(this, _status);
        if (_status.level == kC4Stopped)
            _parent = nullptr;
    }


} }
