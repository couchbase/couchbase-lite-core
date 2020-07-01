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
#include "c4DocEnumerator.h"
#include "c4Private.h"
#include "c4Replicator.h"
#include "c4Database.hh"
#include "c4Certificate.h"
#include "Replicator.hh"
#include "Checkpointer.hh"
#include "Headers.hh"
#include "Error.hh"
#include "Logging.hh"
#include "fleece/Fleece.hh"
#include <atomic>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;


/** Glue between C4 API and internal LiteCore replicator. Abstract class. */
struct C4Replicator : public RefCounted,
                      Logging,
                      Replicator::Delegate,
                      public InstanceCountedIn<C4Replicator>
{
    // Bump this when incompatible changes are made to API or implementation.
    // Subclass c4LocalReplicator is in the couchbase-lite-core-EE repo, which doesn not have a
    // submodule relationship to this one, so it's possible for it to get out of sync.
    static constexpr int API_VERSION = 2;

    virtual void start(bool reset = false) {
        LOCK(_mutex);
        if(_status.level == kC4Stopping) {
            logInfo("Rapid call to start() (stop() is not finished yet), scheduling a restart after stop() is done...");
            _cancelStop = true;
            return;
        }

        if (!_replicator) {
            if(!_start(reset)) {
                UNLOCK();
                // error set as part of _start,
                // but we cannot notify until outside of the lock
                notifyStateChanged();
            }
        }
    }


    // Retry is not supported by default. C4RemoteReplicator overrides this.
    virtual bool retry(bool resetCount, C4Error *outError) {
        c4error_return(LiteCoreDomain, kC4ErrorUnsupported,
                       "Can't retry this type of replication"_sl, outError);
        return false;
    }

    virtual void setHostReachable(bool reachable) {
    }

    void setSuspended(bool suspended) {
        LOCK(_mutex);
        if(_status.level == kC4Stopped) {
            // Suspending a stopped replicator?  Get outta here...
            logInfo("Ignoring a suspend call on a stopped replicator...");
            return;
        }
        
        if(_status.level == kC4Stopping && !statusFlag(kC4Suspended)) {
            // CBL-722: Stop was already called or Replicator is stopped,
            // making suspending meaningless (stop() should override any
            // suspending or unsuspending)
            logInfo("Ignoring a suspend call on a stopping replicator...");
            return;
        }
        
        if(_status.level == kC4Stopping) {
            // CBL-729: At this point, the suspended state has changed from a previous
            // call that caused a suspension to start.  Register to restart later
            // (or cancel the later restart) and move on
            _cancelStop = !suspended;
            if(_cancelStop) {
                logInfo("Request to unsuspend, but Replicator is already suspending.  Will restart after suspending process is completed.");
            } else {
                logInfo("Replicator suspension process being spammed (request to suspend followed by at least one request to unsuspend and then suspend again), attempting to cancel restart.");
            }
            return;
        }
        
        if (!setStatusFlag(kC4Suspended, suspended)) {
            logVerbose("Ignoring redundant suspend call...");
            // Duplicate call, ignore...
            return;
        }
        
        logInfo("%s", (suspended ? "Suspended" : "Un-suspended"));
        if (suspended) {
            _activeWhenSuspended = (_status.level >= kC4Connecting);
            if (_activeWhenSuspended)
                _suspend();
        } else {
            if (_status.level == kC4Offline && _activeWhenSuspended) {
                if(!_unsuspend()) {
                    // error set as part of _unsuspend,
                    // but we cannot notify until outside of the lock
                    UNLOCK();
                    notifyStateChanged();
                }
            }
        }
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
        _cancelStop = false;
        setStatusFlag(kC4Suspended, false);
        if(_status.level == kC4Stopping) {
            // Already stopping, this call is spammy so ignore it
            logVerbose("Duplicate call to stop()...");
            return;
        }
        
        if (_replicator) {
            _status.level = kC4Stopping;
            _replicator->stop();
        } else if (_status.level != kC4Stopped) {
            _status.level = kC4Stopped;
            _status.progress = {};
            UNLOCK();
            notifyStateChanged();
            _selfRetain = nullptr; // balances retain in `_start`
        }
    }

    virtual void setProperties(AllocedDict properties) {
        LOCK(_mutex);
        _options.properties = properties;
    }

    // Prevents any future client callbacks (called by `c4repl_free`.)
    void detach() {
        LOCK(_mutex);
        _onStatusChanged  = nullptr;
        _onDocumentsEnded = nullptr;
        _onBlobProgress   = nullptr;
    }

    bool isDocumentPending(C4Slice docID, C4Error* outErr) const {
        return PendingDocuments(this).isDocumentPending(docID, outErr);
    }

    C4SliceResult pendingDocumentIDs(C4Error* outErr) const {
        return PendingDocuments(this).pendingDocumentIDs(outErr);
    }
    
#ifdef COUCHBASE_ENTERPRISE
    
    C4Cert* getPeerTLSCertificate(C4Error* outErr) const {
        if(!_replicator) {
            return nullptr;
        }
        
        auto cert = _replicator->peerTLSCertificate();
        if(!cert) {
            return nullptr;
        }
        
        
        return c4cert_fromData((C4Slice)cert->data(), outErr);
    }
    
#endif

protected:
    // base constructor
    C4Replicator(C4Database* db NONNULL, const C4ReplicatorParameters &params)
    :Logging(SyncLog)
    ,_database(db)
    ,_options(params)
    ,_onStatusChanged(params.onStatusChanged)
    ,_onDocumentsEnded(params.onDocumentsEnded)
    ,_onBlobProgress(params.onBlobProgress)
    {
        _status.flags |= kC4HostReachable;
    }


    virtual ~C4Replicator() {
        logInfo("Freeing C4Replicator");
        // Tear down the Replicator instance -- this is important in the case where it was
        // never started, because otherwise there will be a bunch of ref cycles that cause many
        // objects (including C4Databases) to be leaked. [CBL-524]
        if (_replicator)
            _replicator->terminate();
    }


    virtual std::string loggingClassName() const override {
        return "C4Replicator";
    }


    bool continuous() const {
        return _options.push == kC4Continuous || _options.pull == kC4Continuous;
    }

    inline bool statusFlag(C4ReplicatorStatusFlags flag) {
        return (_status.flags & flag) != 0;
    }


    bool setStatusFlag(C4ReplicatorStatusFlags flag, bool on) {
        auto flags = _status.flags;
        if (on)
            flags |= flag;
        else
            flags &= ~flag;
        if (flags == _status.flags)
            return false;
        _status.flags = flags;
        return true;
    }


    void updateStatusFromReplicator(C4ReplicatorStatus status) {
        // The Replicator doesn't use the flags, so don't copy them:
        auto flags = _status.flags;
        _status = status;
        _status.flags = flags;
    }


    unsigned getIntProperty(slice key, unsigned defaultValue) const {
        if (auto val = _options.properties[key]; val.type() == kFLNumber)
            return unsigned( max(int64_t(0), min(int64_t(UINT_MAX), val.asInt())) );
        else
            return defaultValue;
    }


    virtual bool createReplicator() =0;

    virtual alloc_slice URL() const =0;


    // Base implementation of starting the replicator.
    // Subclass implementation of `start` must call this (with the mutex locked).
    virtual bool _start(bool reset) {
        if (!_replicator) {
            if(!createReplicator()) {
                return false;
            }
        }

		setStatusFlag(kC4Suspended, false);
        logInfo("Starting Replicator %s", _replicator->loggingName().c_str());
        _selfRetain = this; // keep myself alive till Replicator stops
        updateStatusFromReplicator(_replicator->status());
        _responseHeaders = nullptr;
        _replicator->start(reset);
        return true;
    }

    virtual void _suspend() {
        // called with _mutex locked
        if (_replicator) {
            _status.level = kC4Stopping;
            _replicator->stop();
        }
    }

    virtual bool _unsuspend() {
        // called with _mutex locked
        return _start(false);
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
        bool stopped, resume = false;
        {
            LOCK(_mutex);
            if (repl != _replicator)
                return;
            auto oldLevel = _status.level;
            updateStatusFromReplicator(newStatus);
            if (_status.level > kC4Connecting && oldLevel <= kC4Connecting)
                handleConnected();
            if (_status.level == kC4Stopped) {
                _replicator->terminate();
                _replicator = nullptr;
                if (statusFlag(kC4Suspended)) {
                    // If suspended, go to Offline state when Replicator stops
                    _status.level = kC4Offline;
                } else if(oldLevel != kC4Stopping) {
                    // CBL-1054, only do this if a request to stop is not present, as it should
                    // override the offline handling
                    handleStopped();     // NOTE: handleStopped may change _status
                }
                
                resume = _cancelStop;
                _cancelStop = false;
            }
            stopped = (_status.level == kC4Stopped);
        }
        
        notifyStateChanged();

        if (stopped)
            _selfRetain = nullptr; // balances retain in `_start`
        
        if(resume) {
            start();
        }
    }


    // Replicator::Delegate method, notifying that document(s) have finished.
    virtual void replicatorDocumentsEnded(Replicator *repl,
                          const std::vector<Retained<ReplicatedRev>>& revs) override
    {
        if (repl != _replicator)
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
            if (!docsEnded.empty()) {
                auto onDocsEnded = _onDocumentsEnded.load();
                if (onDocsEnded)
                    onDocsEnded(this, pushing, docsEnded.size(), docsEnded.data(),
                                _options.callbackContext);
            }
        }
    }


    // Replicator::Delegate method, notifying of blob up/download progress.
    virtual void replicatorBlobProgress(Replicator *repl,
                                        const Replicator::BlobProgress &p) override
    {
        if (repl != _replicator)
            return;
        auto onBlob = _onBlobProgress.load();
        if (onBlob)
            onBlob(this, (p.dir == Dir::kPushing),
                   {p.docID.buf, p.docID.size},
                   {p.docProperty.buf, p.docProperty.size},
                   p.key,
                   p.bytesCompleted, p.bytesTotal,
                   p.error,
                   _options.callbackContext);
    }


    // ---- Responding to state changes


    // Called when the replicator's status changes to connected.
    virtual void handleConnected() { }


    // Called when the `Replicator` instance stops, before notifying the client.
    // Subclass override may modify `_status` to change the client notification.
    virtual void handleStopped() { }


    // Posts a notification to the client.
    // The mutex MUST NOT be locked, else if the `onStatusChanged` function calls back into me
    // I will deadlock!
    void notifyStateChanged() {
        C4ReplicatorStatus status = this->status();

        if (willLog()) {
            double progress = 0.0;
            if (status.progress.unitsTotal > 0)
                progress = 100.0 * double(status.progress.unitsCompleted)
                                 / status.progress.unitsTotal;
            if (status.error.code) {
                logError("State: %-s, progress=%.2f%%, error=%s",
                        kC4ReplicatorActivityLevelNames[status.level], progress,
                        c4error_descriptionStr(status.error));
            } else {
                logInfo("State: %-s, progress=%.2f%%",
                      kC4ReplicatorActivityLevelNames[status.level], progress);
            }
        }

        auto onStatusChanged = _onStatusChanged.load();
        if (onStatusChanged && status.level != kC4Stopping /* Don't notify about internal state */)
            onStatusChanged(this, status, _options.callbackContext);
    }


    class PendingDocuments {
    public:
        PendingDocuments(const C4Replicator *repl) {
            // Lock the replicator and copy the necessary state now, so I don't have to lock while
            // calling pendingDocumentIDs (which might call into the app's validation function.)
            LOCK(repl->_mutex);
            replicator = repl->_replicator;
            if (!replicator) {
                checkpointer.emplace(repl->_options, repl->URL());
                database = repl->_database;
            }
        }

        C4SliceResult pendingDocumentIDs(C4Error* outErr) {
            Encoder enc;
            enc.beginArray();

            bool any = false;
            auto callback = [&](const C4DocumentInfo &info) {
                enc.writeString(info.docID);
                any = true;
            };
            bool ok;
            if (replicator)
                ok = replicator->pendingDocumentIDs(callback, outErr);
            else
                ok = checkpointer->pendingDocumentIDs(database, callback, outErr);
            if (!ok)
                return {};

            enc.endArray();
            return any ? C4SliceResult(enc.finish()) : C4SliceResult{};
        }

        bool isDocumentPending(C4Slice docID, C4Error* outErr) {
            if (replicator)
                return replicator->isDocumentPending(docID, outErr);
            else
                return checkpointer->isDocumentPending(database, docID, outErr);
        }

    private:
        Retained<Replicator> replicator;
        optional<Checkpointer> checkpointer;
        Retained<C4Database> database;
    };


    mutable mutex               _mutex;
    Retained<C4Database> const  _database;
    Replicator::Options         _options;

    Retained<Replicator>        _replicator;
    C4ReplicatorStatus          _status {kC4Stopped};
    bool                        _activeWhenSuspended {false};
    bool                        _cancelStop {false};


private:
    alloc_slice                 _responseHeaders;
    Retained<C4Replicator>      _selfRetain;            // Keeps me from being deleted
    atomic<C4ReplicatorStatusChangedCallback>   _onStatusChanged;
    atomic<C4ReplicatorDocumentsEndedCallback>  _onDocumentsEnded;
    atomic<C4ReplicatorBlobProgressCallback>    _onBlobProgress;
};
