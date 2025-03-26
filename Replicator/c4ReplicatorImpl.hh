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
#include "c4Internal.hh"
#include "c4Private.h"
#include "DatabasePool.hh"
#include "Replicator.hh"
#include "Logging.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"  // for AllocedDict
#include <atomic>
#include <variant>
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
        static constexpr int API_VERSION = 5;

        void start(bool reset = false) noexcept override;

        // Retry is not supported by default. C4RemoteReplicator overrides this.
        virtual bool retry(bool resetCount) {
            C4Error::raise(LiteCoreDomain, kC4ErrorUnsupported, "Can't retry this type of replication");
        }

        void setSuspended(bool suspended) noexcept override;

        alloc_slice getResponseHeaders() const noexcept override;

        C4ReplicatorStatus getStatus() const noexcept override;

        void stop() noexcept override;

        virtual void setProperties(AllocedDict properties);

        // Prevents any future client callbacks (called by `c4repl_free`.)
        void stopCallbacks() noexcept override;

        bool isDocumentPending(C4Slice docID, C4CollectionSpec spec) const;

        alloc_slice pendingDocumentIDs(C4CollectionSpec spec) const;

        void setProgressLevel(C4ReplicatorProgressLevel level) noexcept override;

#ifdef COUCHBASE_ENTERPRISE
        void    setPeerTLSCertificateValidator(PeerTLSCertificateValidator) override;
        C4Cert* getPeerTLSCertificate() const override;
#endif

      protected:
        /// Base constructor. For `db` you can pass either a `Retained<C4Database>` or a
        /// `Retained<DatabasePool>`.
        C4ReplicatorImpl(DatabaseOrPool db, const C4ReplicatorParameters& params);
        C4ReplicatorImpl(C4Database*, const C4ReplicatorParameters& params);
        C4ReplicatorImpl(DatabasePool*, const C4ReplicatorParameters& params);

        ~C4ReplicatorImpl() override;

        std::string loggingClassName() const override { return _loggingName; }

        void setLoggingName(const string& loggingName) { _loggingName = loggingName; }

        bool continuous(unsigned collectionIndex = 0) const noexcept;

        inline bool statusFlag(C4ReplicatorStatusFlags flag) const noexcept { return (_status.flags & flag) != 0; }

        bool setStatusFlag(C4ReplicatorStatusFlags flag, bool on) noexcept;

        void updateStatusFromReplicator(C4ReplicatorStatus status) noexcept;

        unsigned getIntProperty(slice key, unsigned defaultValue) const noexcept;

        std::shared_ptr<DBAccess> makeDBAccess(DatabaseOrPool const& dbp, C4DatabaseTag tag) const;

        virtual void createReplicator() = 0;

        virtual alloc_slice URL() const = 0;

        // Base implementation of starting the replicator.
        // Subclass implementation of `start` must call this (with the mutex locked).
        // Rather than throw exceptions, it stores errors in _status.error.
        virtual bool _start(bool reset) noexcept;
        virtual void _suspend() noexcept;
        virtual bool _unsuspend() noexcept;

        // ---- ReplicatorDelegate API:

        // Replicator::Delegate method, notifying that the WebSocket has connected.
        void replicatorGotHTTPResponse(Replicator* repl, int status, const websocket::Headers& headers) override;
        // Replicator::Delegate method, notifying that the status level or progress have changed.
        void replicatorStatusChanged(Replicator* repl, const Replicator::Status& newStatus) override;
        // Replicator::Delegate method, notifying that document(s) have finished.
        void replicatorDocumentsEnded(Replicator* repl, const std::vector<Retained<ReplicatedRev>>& revs) override;
        // Replicator::Delegate method, notifying of blob up/download progress.
        void replicatorBlobProgress(Replicator* repl, const Replicator::BlobProgress& p) override;

        // ---- Responding to state changes

        // Called when the replicator's status changes to connected.
        virtual void handleConnected() {}

        // Called when the `Replicator` instance stops, before notifying the client.
        // Subclass override may modify `_status` to change the client notification.
        virtual void handleStopped() {}

        // Posts a notification to the client.
        // The mutex MUST NOT be locked, else if the `onStatusChanged` function calls back into me
        // I will deadlock!
        void notifyStateChanged() noexcept;

        mutable std::mutex            _mutex;
        DatabaseOrPool const          _database;
        Retained<Replicator::Options> _options;
        PeerTLSCertificateValidator   _peerTLSCertificateValidator;
        Retained<Replicator>          _replicator;
        C4ReplicatorStatus            _status{kC4Stopped};
        bool                          _activeWhenSuspended{false};
        bool                          _cancelStop{false};

      private:
        class PendingDocuments;

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
