//
// C4ReplicatorImpl.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4ReplicatorImpl.hh"
#include "c4DocEnumeratorTypes.h"
#include "c4Certificate.hh"
#include "DatabasePool.hh"
#include "DBAccess.hh"
#include "Checkpointer.hh"
#include "Headers.hh"
#include "Error.hh"
#include <algorithm>
#include <optional>

namespace litecore {

    C4ReplicatorImpl::C4ReplicatorImpl(DatabaseOrPool db, const C4ReplicatorParameters& params)
        : Logging(SyncLog)
        , _database(std::move(db))
        , _options(new Options(params))
        , _loggingName("C4Repl")
        , _onStatusChanged(params.onStatusChanged)
        , _onDocumentsEnded(params.onDocumentsEnded)
        , _onBlobProgress(params.onBlobProgress) {
        _status.flags |= kC4HostReachable;
        _options->verify();
    }

    C4ReplicatorImpl::C4ReplicatorImpl(C4Database* db, const C4ReplicatorParameters& params)
        : C4ReplicatorImpl(DatabaseOrPool(db), params) {}

    C4ReplicatorImpl::C4ReplicatorImpl(DatabasePool* pool, const C4ReplicatorParameters& params)
        : C4ReplicatorImpl(DatabaseOrPool(pool), params) {}

    C4ReplicatorImpl::~C4ReplicatorImpl() {
        logInfo("Freeing C4BaseReplicator");
        // Tear down the Replicator instance -- this is important in the case where it was
        // never started, because otherwise there will be a bunch of ref cycles that cause many
        // objects (including C4Databases) to be leaked. [CBL-524]
        if ( _replicator ) _replicator->terminate();
    }

    void C4ReplicatorImpl::start(bool reset) noexcept {
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

    void C4ReplicatorImpl::setSuspended(bool suspended) noexcept {
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

    alloc_slice C4ReplicatorImpl::getResponseHeaders() const noexcept {
        LOCK(_mutex);
        return _responseHeaders;
    }

    C4ReplicatorStatus C4ReplicatorImpl::getStatus() const noexcept {
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

    void C4ReplicatorImpl::stop() noexcept {
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

    void C4ReplicatorImpl::setProperties(AllocedDict properties) {
        LOCK(_mutex);
        _options->properties = std::move(properties);
    }

    void C4ReplicatorImpl::stopCallbacks() noexcept {
        LOCK(_mutex);
        _onStatusChanged  = nullptr;
        _onDocumentsEnded = nullptr;
        _onBlobProgress   = nullptr;
    }

    void C4ReplicatorImpl::setProgressLevel(C4ReplicatorProgressLevel level) noexcept {
        if ( _options->setProgressLevel(level) ) { logVerbose("Set progress notification level to %d", level); }
    }


#ifdef COUCHBASE_ENTERPRISE
    using PeerTLSCertificateValidator = C4Replicator::PeerTLSCertificateValidator;

    void C4ReplicatorImpl::setPeerTLSCertificateValidator(std::shared_ptr<PeerTLSCertificateValidator> v) {
        std::unique_lock<std::mutex> lock(_peerValidatorMutex);
        _peerTLSCertificateValidator = std::move(v);
    }

    std::shared_ptr<PeerTLSCertificateValidator> C4ReplicatorImpl::getPeerTLSCertificateValidator() const {
        std::unique_lock<std::mutex> lock(_peerValidatorMutex);
        return _peerTLSCertificateValidator;
    }

    C4Cert* C4ReplicatorImpl::getPeerTLSCertificate() const {
        LOCK(_mutex);
        if ( !_peerTLSCertificate && _peerTLSCertificateData ) {
            _peerTLSCertificate     = C4Cert::fromData(_peerTLSCertificateData);
            _peerTLSCertificateData = nullptr;
        }
        return _peerTLSCertificate;
    }

    void C4ReplicatorImpl::_registerBLIPHandlersNow(BLIPHandlerSpecs specs) {
        for ( auto& s : specs )
            _replicator->registerBLIPHandler(std::move(s.profile), s.atBeginning, std::move(s.handler));
    }

    void C4ReplicatorImpl::registerBLIPHandlers(BLIPHandlerSpecs const& specs) {
        LOCK(_mutex);
        if ( _replicator ) _registerBLIPHandlersNow(specs);
        else
            _pendingHandlers.insert(_pendingHandlers.end(), specs.begin(), specs.end());
    }

    void C4ReplicatorImpl::sendBLIPRequest(blip::MessageBuilder& request) {
        LOCK(_mutex);
        _replicator->sendBLIPRequest(request);
    }
#endif

    bool C4ReplicatorImpl::continuous(unsigned collectionIndex) const noexcept {
        return _options->push(collectionIndex) == kC4Continuous || _options->pull(collectionIndex) == kC4Continuous;
    }

    bool C4ReplicatorImpl::setStatusFlag(C4ReplicatorStatusFlags flag, bool on) noexcept {
        auto flags = _status.flags;
        if ( on ) flags |= flag;
        else
            flags &= ~flag;
        if ( flags == _status.flags ) return false;
        _status.flags = flags;
        return true;
    }

    void C4ReplicatorImpl::updateStatusFromReplicator(C4ReplicatorStatus status) noexcept {
        if ( _status.level == kC4Stopping && status.level != kC4Stopped ) {
            // From Stopping it can only go to Stopped
            return;
        }
        // The Replicator doesn't use the flags, so don't copy them:
        auto flags    = _status.flags;
        _status       = status;
        _status.flags = flags;
    }

    unsigned C4ReplicatorImpl::getIntProperty(slice key, unsigned defaultValue) const noexcept {
        if ( auto val = _options->properties[key]; val.type() == kFLNumber ) {
            // CBL-3872: Large unsigned values (higher than max int64) will become
            // negative, and thus get clamped to zero with the old logic, so add
            // special handling for an unsigned fleece value
            if ( val.isUnsigned() ) { return unsigned(std::min(val.asUnsigned(), uint64_t(UINT_MAX))); }

            return unsigned(std::max(int64_t(0), std::min(int64_t(UINT_MAX), val.asInt())));
        }

        return defaultValue;
    }

    std::shared_ptr<DBAccess> C4ReplicatorImpl::makeDBAccess(DatabaseOrPool const& dbp, C4DatabaseTag tag) const {
        bool disableBlobs = _options->properties["disable_blob_support"_sl].asBool();
        if ( auto db = dbp.database() ) {
            auto dbOpenedAgain = db->openAgain();
            _c4db_setDatabaseTag(dbOpenedAgain, tag);
            return make_shared<DBAccess>(dbOpenedAgain, disableBlobs);
        } else {
            return std::make_shared<DBAccess>(dbp.pool(), disableBlobs);
        }
    }

    bool C4ReplicatorImpl::_start(bool reset) noexcept {
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
        logInfo("Starting Replicator %s with config: {%s} and endpoint: %.*s", _replicator->loggingName().c_str(),
                std::string(*_options).c_str(), SPLAT(_replicator->remoteURL()));
        _selfRetain = this;  // keep myself alive till Replicator stops
        updateStatusFromReplicator(_replicator->status());
        _responseHeaders = nullptr;

#ifdef COUCHBASE_ENTERPRISE
        _registerBLIPHandlersNow(std::move(_pendingHandlers));
        _pendingHandlers.clear();
#endif

        _replicator->start(reset);
        return true;
    }

    void C4ReplicatorImpl::_suspend() noexcept {
        // called with _mutex locked
        if ( _replicator ) {
            _status.level = kC4Stopping;
            _replicator->stop();
        }
    }

    bool C4ReplicatorImpl::_unsuspend() noexcept {
        // called with _mutex locked
        return _start(false);
    }

    void C4ReplicatorImpl::replicatorGotTLSCertificate(slice certData) {
#ifdef COUCHBASE_ENTERPRISE
        LOCK(_mutex);
        _peerTLSCertificateData = certData;
        _peerTLSCertificate     = nullptr;
#endif
    }

    void C4ReplicatorImpl::replicatorStatusChanged(Replicator* repl, const Replicator::Status& newStatus) {
        Ref<C4ReplicatorImpl> selfRetain = this;  // Keep myself alive till this method returns

        bool stopped, resume = false;
        {
            LOCK(_mutex);
            if ( repl != _replicator ) return;
            auto oldLevel = _status.level;
            updateStatusFromReplicator((C4ReplicatorStatus)newStatus);
            if ( _status.level > kC4Connecting && oldLevel <= kC4Connecting ) {
                _responseHeaders = _replicator->httpResponse().second.encode();
                handleConnected();
            }
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

    void C4ReplicatorImpl::replicatorDocumentsEnded(Replicator* repl, const std::vector<Ref<ReplicatedRev>>& revs) {
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

    void C4ReplicatorImpl::replicatorBlobProgress(Replicator* repl, const Replicator::BlobProgress& p) {
        if ( repl != _replicator ) return;
        auto onBlob = _onBlobProgress.load();
        if ( onBlob )
            onBlob(this, (p.dir == Dir::kPushing), p.collSpec, p.docID, p.docProperty, p.key, p.bytesCompleted,
                   p.bytesTotal, p.error, _options->callbackContext);
    }

    void C4ReplicatorImpl::notifyStateChanged() noexcept {
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

    class C4ReplicatorImpl::PendingDocuments {
      public:
        static PendingDocuments create(const C4ReplicatorImpl* repl, C4CollectionSpec const& spec) {
            LOCK(repl->_mutex);
            return PendingDocuments(repl, spec);
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
                auto bdb = database.borrow();
                checkpointer->pendingDocumentIDs(bdb, callback);
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
            auto bdb = database.borrow();
            return checkpointer->isDocumentPending(bdb, docID);
        }

      private:
        PendingDocuments(const C4ReplicatorImpl* repl, C4CollectionSpec const& spec)
            : replicator(repl->_replicator)  // safe to copy these since caller locked the repl
            , database(repl->_database)
            , collectionSpec(spec) {
            // CBL-2448: Also make my own checkpointer and database in case a call comes in
            // after Replicator::terminate() is called.  The fix includes the replicator
            // pending document ID function now returning a boolean success, isDocumentPending returning
            // an optional<bool> and if pendingDocumentIDs returns false or isDocumentPending
            // returns nullopt, the checkpointer is fallen back on
            // The collection must be included in the replicator's config options.
            auto it = repl->_options->collectionSpecToIndex().find(collectionSpec);
            if ( it == repl->_options->collectionSpecToIndex().end()
                 || it->second >= repl->_options->workingCollectionCount() ) {
                error::_throw(error::NotOpen, "collection not in the Replicator's config");
            }

            checkpointer.emplace(repl->_options, repl->URL(), collectionSpec);
        }

        Retained<Replicator>   replicator;
        optional<Checkpointer> checkpointer;  // initialized in the constructor
        DatabaseOrPool         database;
        C4CollectionSpec       collectionSpec;
    };

    bool C4ReplicatorImpl::isDocumentPending(C4Slice docID, C4CollectionSpec spec) const {
        return PendingDocuments::create(this, spec).isDocumentPending(docID);
    }

    alloc_slice C4ReplicatorImpl::pendingDocumentIDs(C4CollectionSpec spec) const {
        return PendingDocuments::create(this, spec).pendingDocumentIDs();
    }
}  // namespace litecore
