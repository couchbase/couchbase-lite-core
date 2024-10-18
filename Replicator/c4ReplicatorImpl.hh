//
// C4ReplicatorImpl.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "c4Replicator.hh"
#include "c4Database.hh"
#include "c4DocEnumeratorTypes.h"
#include "c4Certificate.hh"
#include "c4Internal.hh"
#include "Replicator.hh"
#include "Checkpointer.hh"
#include "Headers.hh"
#include "Error.hh"
#include "Logging.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"  // for AllocedDict
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

namespace litecore {

    using namespace fleece;
    using namespace litecore;
    using namespace litecore::repl;

    /** Abstract subclass of the public C4Replicator that implements common functionality. */
    struct C4ReplicatorImpl
        : public C4Replicator
        , Logging
        , Replicator::Delegate
        , public InstanceCountedIn<C4ReplicatorImpl> {
        // Bump this when incompatible changes are made to API or implementation.
        // Subclass c4LocalReplicator is in the couchbase-lite-core-EE repo, which doesn not have a
        // submodule relationship to this one, so it's possible for it to get out of sync.
        static constexpr int API_VERSION = 4;

        void start(bool reset = false) noexcept override {
            LOCK(_mutex);
            if ( _status.level == kC4Stopping ) {
                logInfo("Rapid call to start() (stop() is not finished yet), scheduling a restart after stop() is "
                        "done...");
                _cancelStop = true;
                return;
            }

            if ( !_replicator ) {
                if ( !_start(reset) ) {
                    UNLOCK();
                    // error set as part of _start,
                    // but we cannot notify until outside of the lock
                    notifyStateChanged();
                }
            }
        }

        // Retry is not supported by default. C4RemoteReplicator overrides this.
        virtual bool retry(bool resetCount) {
            C4Error::raise(LiteCoreDomain, kC4ErrorUnsupported, "Can't retry this type of replication");
        }

        void setSuspended(bool suspended) noexcept override {
            LOCK(_mutex);
            if ( _status.level == kC4Stopped ) {
                // Suspending a stopped replicator?  Get outta here...
                logInfo("Ignoring a suspend call on a stopped replicator...");
                return;
            }

            if ( _status.level == kC4Stopping && !statusFlag(kC4Suspended) ) {
                // CBL-722: Stop was already called or Replicator is stopped,
                // making suspending meaningless (stop() should override any
                // suspending or unsuspending)
                logInfo("Ignoring a suspend call on a stopping replicator...");
                return;
            }

            if ( _status.level == kC4Stopping ) {
                // CBL-729: At this point, the suspended state has changed from a previous
                // call that caused a suspension to start.  Register to restart later
                // (or cancel the later restart) and move on
                _cancelStop = !suspended;
                if ( _cancelStop ) {
                    logInfo("Request to unsuspend, but Replicator is already suspending.  Will restart after "
                            "suspending process is completed.");
                } else {
                    logInfo("Replicator suspension process being spammed (request to suspend followed by at least one "
                            "request to unsuspend and then suspend again), attempting to cancel restart.");
                }
                return;
            }

            if ( !setStatusFlag(kC4Suspended, suspended) ) {
                logVerbose("Ignoring redundant suspend call...");
                // Duplicate call, ignore...
                return;
            }

            logInfo("%s", (suspended ? "Suspended" : "Un-suspended"));
            if ( suspended ) {
                _activeWhenSuspended = (_status.level >= kC4Connecting);
                if ( _activeWhenSuspended ) _suspend();
            } else {
                if ( _status.level == kC4Offline && _activeWhenSuspended ) {
                    if ( !_unsuspend() ) {
                        // error set as part of _unsuspend,
                        // but we cannot notify until outside of the lock
                        UNLOCK();
                        notifyStateChanged();
                    }
                }
            }
        }

        alloc_slice getResponseHeaders() const noexcept override {
            LOCK(_mutex);
            return _responseHeaders;
        }

        C4ReplicatorStatus getStatus() const noexcept override {
            LOCK(_mutex);
            switch ( _status.level ) {
                // CBL-1513: Any new approved statuses must be added to this list,
                // or they will be forced to busy in order to prevent internal statuses
                // from leaking
                case kC4Busy:
                case kC4Connecting:
                case kC4Idle:
                case kC4Offline:
                case kC4Stopped:
                    return _status;

                default:
                    return {kC4Busy, _status.progress, _status.error, _status.flags};
            }
        }

        void stop() noexcept override {
            LOCK(_mutex);
            _cancelStop = false;
            setStatusFlag(kC4Suspended, false);
            if ( _status.level == kC4Stopping ) {
                // Already stopping, this call is spammy so ignore it
                logVerbose("Duplicate call to stop()...");
                return;
            }

            if ( _replicator ) {
                _status.level = kC4Stopping;
                _replicator->stop();
            } else if ( _status.level != kC4Stopped ) {
                _status.level    = kC4Stopped;
                _status.progress = {};
                UNLOCK();
                notifyStateChanged();
                _selfRetain = nullptr;  // balances retain in `_start` -- may destruct me!
            }
        }

        virtual void setProperties(AllocedDict properties) {
            LOCK(_mutex);
            _options->properties = std::move(properties);
        }

        // Prevents any future client callbacks (called by `c4repl_free`.)
        void stopCallbacks() noexcept override {
            LOCK(_mutex);
            _onStatusChanged  = nullptr;
            _onDocumentsEnded = nullptr;
            _onBlobProgress   = nullptr;
        }

        bool isDocumentPending(C4Slice docID, C4CollectionSpec spec) const {
            return PendingDocuments(this, spec).isDocumentPending(docID);
        }

        alloc_slice pendingDocumentIDs(C4CollectionSpec spec) const {
            return PendingDocuments(this, spec).pendingDocumentIDs();
        }

        void setProgressLevel(C4ReplicatorProgressLevel level) noexcept override {
            if ( _options->setProgressLevel(level) ) { logVerbose("Set progress notification level to %d", level); }
        }

#ifdef COUCHBASE_ENTERPRISE

        C4Cert* getPeerTLSCertificate() const override {
            LOCK(_mutex);
            if ( !_peerTLSCertificate && _peerTLSCertificateData ) {
                _peerTLSCertificate     = C4Cert::fromData(_peerTLSCertificateData);
                _peerTLSCertificateData = nullptr;
            }
            return _peerTLSCertificate;
        }

#endif

      protected:
        // base constructor
        C4ReplicatorImpl(C4Database* db NONNULL, const C4ReplicatorParameters& params)
            : Logging(SyncLog)
            , _database(db)
            , _options(new Options(params))
            , _onStatusChanged(params.onStatusChanged)
            , _onDocumentsEnded(params.onDocumentsEnded)
            , _onBlobProgress(params.onBlobProgress)
            , _loggingName("C4Repl") {
            _status.flags |= kC4HostReachable;
            _options->verify();
        }

        ~C4ReplicatorImpl() override {
            logInfo("Freeing C4BaseReplicator");
            // Tear down the Replicator instance -- this is important in the case where it was
            // never started, because otherwise there will be a bunch of ref cycles that cause many
            // objects (including C4Databases) to be leaked. [CBL-524]
            if ( _replicator ) _replicator->terminate();
        }

        std::string loggingClassName() const override { return _loggingName; }

        bool continuous(unsigned collectionIndex = 0) const noexcept {
            return _options->push(collectionIndex) == kC4Continuous || _options->pull(collectionIndex) == kC4Continuous;
        }

        inline bool statusFlag(C4ReplicatorStatusFlags flag) const noexcept { return (_status.flags & flag) != 0; }

        bool setStatusFlag(C4ReplicatorStatusFlags flag, bool on) noexcept {
            auto flags = _status.flags;
            if ( on ) flags |= flag;
            else
                flags &= ~flag;
            if ( flags == _status.flags ) return false;
            _status.flags = flags;
            return true;
        }

        void updateStatusFromReplicator(C4ReplicatorStatus status) noexcept {
            if ( _status.level == kC4Stopping && status.level != kC4Stopped ) {
                // From Stopping it can only go to Stopped
                return;
            }
            // The Replicator doesn't use the flags, so don't copy them:
            auto flags    = _status.flags;
            _status       = status;
            _status.flags = flags;
        }

        unsigned getIntProperty(slice key, unsigned defaultValue) const noexcept {
            if ( auto val = _options->properties[key]; val.type() == kFLNumber ) {
                // CBL-3872: Large unsigned values (higher than max int64) will become
                // negative, and thus get clamped to zero with the old logic, so add
                // special handling for an unsigned fleece value
                if ( val.isUnsigned() ) { return unsigned(std::min(val.asUnsigned(), uint64_t(UINT_MAX))); }

                return unsigned(std::max(int64_t(0), std::min(int64_t(UINT_MAX), val.asInt())));
            }

            return defaultValue;
        }

        virtual void createReplicator() = 0;

        virtual alloc_slice URL() const = 0;

        // Base implementation of starting the replicator.
        // Subclass implementation of `start` must call this (with the mutex locked).
        // Rather than throw exceptions, it stores errors in _status.error.
        virtual bool _start(bool reset) noexcept {
            if ( !_replicator ) {
                try {
                    createReplicator();
                } catch ( exception& x ) {
                    _status.error = C4Error::fromException(x);
                    _replicator   = nullptr;
                    return false;
                }
            }

            setStatusFlag(kC4Suspended, false);
            logInfo("Starting Replicator %s with config: {%s}\n", _replicator->loggingName().c_str(),
                    std::string(*_options).c_str());
            _selfRetain = this;  // keep myself alive till Replicator stops
            updateStatusFromReplicator(_replicator->status());
            _responseHeaders = nullptr;
            _replicator->start(reset);
            return true;
        }

        virtual void _suspend() noexcept {
            // called with _mutex locked
            if ( _replicator ) {
                _status.level = kC4Stopping;
                _replicator->stop();
            }
        }

        virtual bool _unsuspend() noexcept {
            // called with _mutex locked
            return _start(false);
        }

        // ---- ReplicatorDelegate API:


        // Replicator::Delegate method, notifying that the WebSocket has connected.
        void replicatorGotHTTPResponse(Replicator* repl, int status, const websocket::Headers& headers) override {
            LOCK(_mutex);
            if ( repl == _replicator ) {
                Assert(!_responseHeaders);
                _responseHeaders = headers.encode();
            }
        }

        void replicatorGotTLSCertificate(slice certData) override {
#ifdef COUCHBASE_ENTERPRISE
            LOCK(_mutex);
            _peerTLSCertificateData = certData;
            _peerTLSCertificate     = nullptr;
#endif
        }

        // Replicator::Delegate method, notifying that the status level or progress have changed.
        void replicatorStatusChanged(Replicator* repl, const Replicator::Status& newStatus) override {
            Retained<C4ReplicatorImpl> selfRetain = this;  // Keep myself alive till this method returns

            bool stopped, resume = false;
            {
                LOCK(_mutex);
                if ( repl != _replicator ) return;
                auto oldLevel = _status.level;
                updateStatusFromReplicator((C4ReplicatorStatus)newStatus);
                if ( _status.level > kC4Connecting && oldLevel <= kC4Connecting ) handleConnected();
                if ( _status.level == kC4Stopped ) {
                    _replicator->terminate();
                    _replicator = nullptr;
                    if ( statusFlag(kC4Suspended) ) {
                        // If suspended, go to Offline state when Replicator stops
                        _status.level = kC4Offline;
                    } else if ( oldLevel != kC4Stopping ) {
                        // CBL-1054, only do this if a request to stop is not present, as it should
                        // override the offline handling
                        handleStopped();  // NOTE: handleStopped may change _status
                    }

                    resume      = _cancelStop;
                    _cancelStop = false;
                }
                stopped = (_status.level == kC4Stopped);
            }

            notifyStateChanged();

            if ( stopped ) _selfRetain = nullptr;  // balances retain in `_start`

            if ( resume ) { start(); }

            // On return from this method, if I stopped I may be deleted (due to clearing _selfRetain)
        }

        // Replicator::Delegate method, notifying that document(s) have finished.
        void replicatorDocumentsEnded(Replicator* repl, const std::vector<Retained<ReplicatedRev>>& revs) override {
            if ( repl != _replicator ) return;

            auto                                nRevs = revs.size();
            std::vector<const C4DocumentEnded*> docsEnded;
            docsEnded.reserve(nRevs);
            for ( int pushing = 0; pushing <= 1; ++pushing ) {
                docsEnded.clear();
                for ( const auto& rev : revs ) {
                    if ( (rev->dir() == Dir::kPushing) == pushing ) docsEnded.push_back(rev->asDocumentEnded());
                }
                if ( !docsEnded.empty() ) {
                    auto onDocsEnded = _onDocumentsEnded.load();
                    if ( onDocsEnded )
                        onDocsEnded(this, pushing, docsEnded.size(), docsEnded.data(), _options->callbackContext);
                }
            }
        }

        // Replicator::Delegate method, notifying of blob up/download progress.
        void replicatorBlobProgress(Replicator* repl, const Replicator::BlobProgress& p) override {
            if ( repl != _replicator ) return;
            auto onBlob = _onBlobProgress.load();
            if ( onBlob )
                onBlob(this, (p.dir == Dir::kPushing), p.collSpec, p.docID, p.docProperty, p.key, p.bytesCompleted,
                       p.bytesTotal, p.error, _options->callbackContext);
        }

        // ---- Responding to state changes


        // Called when the replicator's status changes to connected.
        virtual void handleConnected() {}

        // Called when the `Replicator` instance stops, before notifying the client.
        // Subclass override may modify `_status` to change the client notification.
        virtual void handleStopped() {}

        // Posts a notification to the client.
        // The mutex MUST NOT be locked, else if the `onStatusChanged` function calls back into me
        // I will deadlock!
        void notifyStateChanged() noexcept {
            C4ReplicatorStatus status = this->getStatus();

            if ( willLog() ) {
                double progress = 0.0;
                if ( status.progress.unitsTotal > 0 )
                    progress = 100.0 * double(status.progress.unitsCompleted) / double(status.progress.unitsTotal);
                if ( status.error.code ) {
                    logError("State: %-s, progress=%.2f%%, error=%s", kC4ReplicatorActivityLevelNames[status.level],
                             progress, status.error.description().c_str());
                } else {
                    logInfo("State: %-s, progress=%.2f%%", kC4ReplicatorActivityLevelNames[status.level], progress);
                }
            }

            if ( !(status.error.code && status.level > kC4Offline) ) {
                auto onStatusChanged = _onStatusChanged.load();
                if ( onStatusChanged && status.level != kC4Stopping /* Don't notify about internal state */ )
                    onStatusChanged(this, status, _options->callbackContext);
            }
        }

        class PendingDocuments {
          public:
            PendingDocuments(const C4ReplicatorImpl* repl, C4CollectionSpec spec) : collectionSpec(spec) {
                // Lock the replicator and copy the necessary state now, so I don't have to lock while
                // calling pendingDocumentIDs (which might call into the app's validation function.)
                LOCK(repl->_mutex);
                replicator = repl->_replicator;

                // CBL-2448: Also make my own checkpointer and database in case a call comes in
                // after Replicator::terminate() is called.  The fix includes the replicator
                // pending document ID function now returning a boolean success, isDocumentPending returning
                // an optional<bool> and if pendingDocumentIDs returns false or isDocumentPending
                // returns nullopt, the checkpointer is fallen back on
                C4Collection* collection = nullptr;
                // The collection must be included in the replicator's config options.
                auto it = repl->_options->collectionSpecToIndex().find(collectionSpec);
                if ( it != repl->_options->collectionSpecToIndex().end() ) {
                    if ( it->second < repl->_options->workingCollectionCount() ) {
                        collection = repl->_database->getCollection(collectionSpec);
                    }
                }

                if ( collection == nullptr ) {
                    error::_throw(error::NotOpen, "collection not in the Replicator's config");
                }

                checkpointer = new Checkpointer{repl->_options, repl->URL(), collection};
                database     = repl->_database;
            }

            alloc_slice pendingDocumentIDs() {
                Encoder enc;
                enc.beginArray();
                bool any      = false;
                auto callback = [&](const C4DocumentInfo& info) {
                    enc.writeString(info.docID);
                    any = true;
                };

                if ( !replicator || !replicator->pendingDocumentIDs(collectionSpec, callback) ) {
                    checkpointer->pendingDocumentIDs(database, callback);
                }

                if ( !any ) return {};
                enc.endArray();
                return enc.finish();
            }

            bool isDocumentPending(C4Slice docID) {
                if ( replicator ) {
                    auto result = replicator->isDocumentPending(docID, collectionSpec);
                    if ( result.has_value() ) { return *result; }
                }
                return checkpointer->isDocumentPending(database, docID);
            }

          private:
            Retained<Replicator> replicator;
            Checkpointer*        checkpointer{nullptr};  // assigned in the constructor
            Retained<C4Database> database;
            C4CollectionSpec     collectionSpec;
        };

        mutable std::mutex            _mutex;
        Retained<C4Database> const    _database;
        Retained<Replicator::Options> _options;

        Retained<Replicator> _replicator;
        C4ReplicatorStatus   _status{kC4Stopped};
        bool                 _activeWhenSuspended{false};
        bool                 _cancelStop{false};

      protected:
        void setLoggingName(const string& loggingName) { _loggingName = loggingName; }

      private:
        std::string _loggingName;
        alloc_slice _responseHeaders;
#ifdef COUCHBASE_ENTERPRISE
        mutable alloc_slice      _peerTLSCertificateData;
        mutable Retained<C4Cert> _peerTLSCertificate;
#endif
        Retained<C4ReplicatorImpl>                      _selfRetain;  // Keeps me from being deleted
        std::atomic<C4ReplicatorStatusChangedCallback>  _onStatusChanged;
        std::atomic<C4ReplicatorDocumentsEndedCallback> _onDocumentsEnded;
        std::atomic<C4ReplicatorBlobProgressCallback>   _onBlobProgress;
    };

}  // namespace litecore
