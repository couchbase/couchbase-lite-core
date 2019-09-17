//
// c4Replicator.hh
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

#pragma once

#include "fleece/Fleece.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Replicator.h"
#include "Database.hh"
#include "Replicator.hh"
#include "Headers.hh"
#include "Error.hh"
#include "Logging.hh"

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;


#define LOCK(MUTEX)     lock_guard<mutex> _lock(MUTEX)


static const char *kReplicatorProtocolName = "+CBMobile_2";


/** Glue between C4 API and internal LiteCore replicator. Abstract class. */
struct C4Replicator : public RefCounted, Logging, Replicator::Delegate {

    // Subclass must override this, creating a Replicator instance and passing it to `_start`.
    virtual void start(bool synchronous =false) =0;

    // Retry is not supported by default. C4RemoteReplicator overrides this.
    virtual bool retry(bool resetCount, C4Error *outError) {
        c4error_return(LiteCoreDomain, kC4ErrorUnsupported,
                       "Can't retry this type of replication"_sl, outError);
        return false;
    }

    virtual bool willRetry() const {
        return false;
    }

    alloc_slice responseHeaders() {
        LOCK(_mutex);
        return _responseHeaders;
    }

    C4ReplicatorStatus status() {
        LOCK(_mutex);
        return _status;
    }

    virtual void stop() {
        LOCK(_mutex);
        if (_replicator)
            _replicator->stop();
    }

    // Prevents any future client callbacks (called by `c4repl_free`.)
    void detach() {
        LOCK(_mutex);
        _params.onStatusChanged  = nullptr;
        _params.onDocumentsEnded = nullptr;
        _params.onBlobProgress   = nullptr;
        _params.callbackContext  = nullptr;
    }

    bool continuous() const {
        return _params.push == kC4Continuous || _params.pull == kC4Continuous;
    }

protected:
    // base constructor
    C4Replicator(C4Database* db, const C4ReplicatorParameters &params)
    :Logging(SyncLog)
    ,_database(db)
    ,_params(params)
    { }


    virtual std::string loggingClassName() const override {
        return "C4Replicator";
    }


    // Returns the `Options` to pass to the `Replicator` instance
    Replicator::Options options() const {
        Replicator::Options opts(_params.push, _params.pull, _params.optionsDictFleece);
        opts.pushFilter = _params.pushFilter;
        opts.pullValidator = _params.validationFunc;
        opts.callbackContext = _params.callbackContext;
        return opts;
    }


    // Base implementation of starting the replicator.
    // Subclass implementation of `start` must call this (with the mutex locked).
    virtual void _start(Replicator *replicator, bool synchronous =false) {
        logInfo("Starting Replicator %s", replicator->loggingName().c_str());
        DebugAssert(!_replicator);
        _status = replicator->status();
        _selfRetain = this; // keep myself alive till Replicator stops
        _replicator = replicator;
        _replicator->start(synchronous);
    }


    // ---- ReplicatorDelegate API:


    // Replicator::Delegate method, notifying that the WebSocket has connected.
    virtual void replicatorGotHTTPResponse(Replicator *repl, int status,
                                           const websocket::Headers &headers) override
    {
        LOCK(_mutex);
        if (repl == _replicator) {
            Assert(!_responseHeaders);
            _responseHeaders = headers.encode();
        }
    }


    // Replicator::Delegate method, notifying that the status level or progress have changed.
    virtual void replicatorStatusChanged(Replicator *repl,
                                         const Replicator::Status &newStatus) override
    {
        bool stopped;
        {
            LOCK(_mutex);
            if (repl != _replicator)
                return;
            auto oldLevel = _status.level;
            _status = newStatus;
            if (_status.level > kC4Connecting && oldLevel <= kC4Connecting)
                handleConnected();
            if (_status.level == kC4Stopped && _status.error.code)
                handleError(_status.error);     // NOTE: handleError may change _status
            stopped = (_status.level == kC4Stopped);
        }
        
        notifyStateChanged();

        if (stopped)
            _selfRetain = nullptr; // balances retain in constructor
    }


    // Replicator::Delegate method, notifying that document(s) have finished.
    virtual void replicatorDocumentsEnded(Replicator *repl,
                          const std::vector<Retained<ReplicatedRev>>& revs) override
    {
        if (repl != _replicator)
            return;
        C4ReplicatorDocumentsEndedCallback onDocsEnded;
        {
            LOCK(_mutex);
            onDocsEnded = _params.onDocumentsEnded;
        }
        if (!onDocsEnded)
            return;

        auto nRevs = revs.size();
        vector<const C4DocumentEnded*> docsEnded;
        docsEnded.reserve(nRevs);
        for (int pushing = 0; pushing <= 1; ++pushing) {
            docsEnded.clear();
            for (auto rev : revs) {
                if ((rev->dir() == Dir::kPushing) == pushing)
                    docsEnded.push_back(rev->asDocumentEnded());
            }
            if (!docsEnded.empty())
                onDocsEnded(this, pushing, docsEnded.size(), docsEnded.data(), _params.callbackContext);
        }
    }


    // Replicator::Delegate method, notifying of blob up/download progress.
    virtual void replicatorBlobProgress(Replicator *repl,
                                        const Replicator::BlobProgress &p) override
    {
        if (repl != _replicator)
            return;
        C4ReplicatorBlobProgressCallback onBlob;
        {
            LOCK(_mutex);
            onBlob = _params.onBlobProgress;
        }
        if (onBlob)
            onBlob(this, (p.dir == Dir::kPushing),
                   {p.docID.buf, p.docID.size},
                   {p.docProperty.buf, p.docProperty.size},
                   p.key,
                   p.bytesCompleted, p.bytesTotal,
                   p.error,
                   _params.callbackContext);
    }


    // ---- Responding to state changes


    // Called when the replicator's status changes to connected.
    virtual void handleConnected() { }


    // Called when the `Replicator` instance stops with an error, before notifying the client.
    // Subclass override may modify `_status` to change the client notification.
    virtual void handleError(C4Error) { }


    // Posts a notification to the client.
    // The mutex MUST NOT be locked, else if the `onStatusChanged` function calls back into me
    // I will deadlock!
    void notifyStateChanged() {
        if (willLog()) {
            double progress = 0.0;
            if (_status.progress.unitsTotal > 0)
                progress = 100.0 * double(_status.progress.unitsCompleted)
                                 / _status.progress.unitsTotal;
            if (_status.error.code) {
                logError("State: %-s, progress=%.2f%%, error=%s",
                        kC4ReplicatorActivityLevelNames[_status.level], progress,
                        c4error_descriptionStr(_status.error));
            } else {
                logInfo("State: %-s, progress=%.2f%%",
                      kC4ReplicatorActivityLevelNames[_status.level], progress);
            }
        }

        C4ReplicatorStatusChangedCallback onStatusChanged;
        {
            LOCK(_mutex);
            onStatusChanged = _params.onStatusChanged;
        }
        if (onStatusChanged)
            onStatusChanged(this, _status, _params.callbackContext);
    }

    mutex _mutex;
    Retained<C4Database> const  _database;
    C4ReplicatorParameters      _params;

    Retained<Replicator>        _replicator;
    alloc_slice                 _responseHeaders;
    C4ReplicatorStatus          _status {kC4Stopped};
    Retained<C4Replicator>      _selfRetain;            // Keeps me from being deleted
};
