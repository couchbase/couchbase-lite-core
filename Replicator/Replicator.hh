//
// Replicator.hh
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
#include "Worker.hh"
#include "Checkpointer.hh"
#include "BLIPConnection.hh"
#include "Batcher.hh"
#include "Stopwatch.hh"
#include "c4DatabaseTypes.h"
#include <array>
#include <optional>
#include <utility>

namespace litecore::repl {
    class Pusher;
    class Puller;
    class ReplicatedRev;

    static const array<string, 2> kCompatProtocols = {{string(blip::Connection::kWSProtocolName) + "+CBMobile_3",
                                                       string(blip::Connection::kWSProtocolName) + "+CBMobile_2"}};

    /** The top-level replicator object, which runs the BLIP connection.
        Pull and push operations are run by subidiary Puller and Pusher objects.
        The database will only be accessed by the DBAgent object. */
    class Replicator final
        : public Worker
        , private blip::ConnectionDelegate {
        friend class WeakHolder<blip::ConnectionDelegate>;

      public:
        class Delegate;
        using CloseStatus = blip::Connection::CloseStatus;
        using Options     = litecore::repl::Options;

        Replicator(C4Database* NONNULL, websocket::WebSocket* NONNULL, Delegate&, Options* NONNULL);
        Replicator(const shared_ptr<DBAccess>&, websocket::WebSocket* NONNULL, Delegate&, Options* NONNULL);

        struct BlobProgress {
            Dir              dir;
            C4CollectionSpec collSpec;
            alloc_slice      docID;
            alloc_slice      docProperty;
            C4BlobKey        key;
            uint64_t         bytesCompleted;
            uint64_t         bytesTotal;
            C4Error          error;
        };

        using DocumentsEnded = std::vector<Retained<ReplicatedRev>>;

        static std::string ProtocolName();

        /** Replicator delegate; receives progress & error notifications. */
        class Delegate {
          public:
            virtual ~Delegate() = default;

            virtual void replicatorGotHTTPResponse(Replicator* NONNULL, int status, const websocket::Headers& headers) {
            }

            virtual void replicatorGotTLSCertificate(slice certData)                 = 0;
            virtual void replicatorStatusChanged(Replicator* NONNULL, const Status&) = 0;

            virtual void replicatorConnectionClosed(Replicator* NONNULL, const CloseStatus&) {}

            virtual void replicatorDocumentsEnded(Replicator* NONNULL, const DocumentsEnded&) = 0;
            virtual void replicatorBlobProgress(Replicator* NONNULL, const BlobProgress&)     = 0;
        };

        void start(bool reset = false, bool synchronous = false);

        void stop() { enqueue(FUNCTION_TO_QUEUE(Replicator::_stop)); }

        /** Tears down a Replicator state including any reference cycles.
            The Replicator must have either already stopped, or never started.
            No further delegate callbacks will be made!  */
        void terminate();

        /** Invokes the callback for each document which has revisions pending push.
            Returns false if unable to do so.  */
        bool pendingDocumentIDs(C4CollectionSpec, Checkpointer::PendingDocCallback);

        /** Checks if the document with the given ID has any pending revisions to push.  If unable, returns an empty optional. */
        std::optional<bool> isDocumentPending(slice docID, C4CollectionSpec);

        Checkpointer& checkpointer(CollectionIndex coll) { return *_subRepls[coll].checkpointer; }

        void endedDocument(ReplicatedRev* d NONNULL);

        void onBlobProgress(const BlobProgress& progress) {
            enqueue(FUNCTION_TO_QUEUE(Replicator::_onBlobProgress), progress);
        }

        void docRemoteAncestorChanged(alloc_slice docID, alloc_slice revID, CollectionIndex);

        Retained<Replicator> replicatorIfAny() override { return this; }

        // exposed for unit tests:
        websocket::WebSocket* webSocket() const { return connection().webSocket(); }

        C4Collection* collection(CollectionIndex i) const {
            Assert(i < _subRepls.size());
            return _subRepls[i].collection;
        }

      protected:
        std::string loggingClassName() const override { return _options->isActive() ? "Repl" : "repl"; }

        // Replicator owns multiple subRepls, so it doesn't use _collectionIndex, and therefore we must
        // pass the collectionIndex manually for log calls
        template <class... Args>
        inline void cLogInfo(CollectionIndex idx, const char* fmt, Args... args) const {
            const char* fmt_ = formatWithCollection(fmt);
            Logging::logInfo(fmt_, idx, args...);
        }

        template <class... Args>
        inline void cLogVerbose(CollectionIndex idx, const char* fmt, Args... args) const {
            const char* fmt_ = formatWithCollection(fmt);
            Logging::logVerbose(fmt_, idx, args...);
        }
#if DEBUG
        template <class... Args>
        inline void cLogDebug(CollectionIndex idx, const char* fmt, Args... args) const {
            const char* fmt_ = formatWithCollection(fmt);
            Logging::logVerbose(fmt_, idx, args...);
        }
#else
        template <class... Args>
        inline void cLogDebug(CollectionIndex idx, const char* fmt, Args... args) const {}
#endif

        // BLIP ConnectionDelegate API:
        void onHTTPResponse(int status, const websocket::Headers& headers) override;
        void onTLSCertificate(slice certData) override;

        void onConnect() override { enqueue(FUNCTION_TO_QUEUE(Replicator::_onConnect)); }

        void onClose(CloseStatus status, blip::Connection::State state) override {
            enqueue(FUNCTION_TO_QUEUE(Replicator::_onClose), status, state);
        }

        void onRequestReceived(blip::MessageIn* msg NONNULL) override {
            enqueue(FUNCTION_TO_QUEUE(Replicator::_onRequestReceived), retained(msg));
        }

        void changedStatus() override;

        void onError(C4Error error) override;

        // Worker method overrides:
        ActivityLevel computeActivityLevel() const override;
        void          _childChangedStatus(Retained<Worker>, Status taskStatus) override;

      private:
        void _onHTTPResponse(int status, websocket::Headers headers);
        void _onConnect();
        void _onError(int errcode, fleece::alloc_slice reason);
        void _onClose(CloseStatus, blip::Connection::State);
        void _onRequestReceived(Retained<blip::MessageIn> msg);

        void _start(bool reset);
        void _stop();
        void _disconnect(websocket::CloseCode closeCode, slice message);
        void _findExistingConflicts();
        bool getLocalCheckpoint(bool reset, CollectionIndex);
        void getRemoteCheckpoint(bool refresh, CollectionIndex);
        void getCollections();
        void startReplicating(CollectionIndex);
        void reportStatus();

        void updateCheckpoint();

        void saveCheckpoint(CollectionIndex coll, alloc_slice json) {
            enqueue(FUNCTION_TO_QUEUE(Replicator::_saveCheckpoint), coll, std::move(json));
        }

        void _saveCheckpoint(CollectionIndex, alloc_slice json);
        void saveCheckpointNow(CollectionIndex);

        void notifyEndedDocuments(int gen = actor::AnyGen);
        void _onBlobProgress(BlobProgress);

        // Checkpoints:
        void        checkpointIsInvalid();
        std::string remoteDBIDString() const;
        void        handleGetCheckpoint(Retained<blip::MessageIn>);
        void        handleSetCheckpoint(Retained<blip::MessageIn>);
        void        handleGetCollections(Retained<blip::MessageIn>);
        void        returnForbidden(Retained<blip::MessageIn>);
        slice       getPeerCheckpointDocID(blip::MessageIn* request, const char* whatFor) const;

        string statusVString() const;
        void   updatePushStatus(CollectionIndex i, const Status& status);
        void   updatePullStatus(CollectionIndex i, const Status& status);
        void   prepareWorkers();

        void delegateCollectionSpecificMessageToWorker(Retained<blip::MessageIn>);

      public:
        template <typename WORKER>
        void registerWorkerHandler(WORKER* worker, const char* profile NONNULL,
                                   void (WORKER::*method)(Retained<blip::MessageIn>)) {
            std::function<void(Retained<blip::MessageIn>)> fn(std::bind(method, worker, std::placeholders::_1));
            pair<string, CollectionIndex>                  key{profile, worker->collectionIndex()};
            _workerHandlers.emplace(key, worker->asynchronize(profile, fn));
        }

      private:
        using WorkerHandler  = std::function<void(Retained<blip::MessageIn>)>;
        using WorkerHandlers = std::map<pair<string, CollectionIndex>, blip::Connection::RequestHandler>;
        WorkerHandlers _workerHandlers;

        // Member variables:

        struct SubReplicator {
            Retained<Pusher>         pusher;
            Retained<Puller>         puller;
            Status                   pushStatus;                        // Current status of Pusher
            Status                   pullStatus;                        // Current status of Puller
            unique_ptr<Checkpointer> checkpointer;                      // Object that manages checkpoints
            bool                     hadLocalCheckpoint{false};         // True if local checkpoint pre-existed
            bool                     remoteCheckpointRequested{false};  // True while "getCheckpoint" request pending
            bool                     remoteCheckpointReceived{false};   // True if I got a "getCheckpoint" response
            alloc_slice              checkpointJSONToSave;              // JSON waiting to be saved to the checkpts
            alloc_slice              remoteCheckpointDocID;             // Checkpoint docID to use with peer
            alloc_slice              remoteCheckpointRevID;             // Latest revID of remote checkpoint
            Retained<C4Collection>   collection;
        };

        using ReplicatedRevBatcher = actor::ActorBatcher<Replicator, ReplicatedRev>;

        void setMsgHandlerFor3_0_Client(const Retained<blip::MessageIn>&);

        Delegate*               _delegate;         // Delegate whom I report progress/errors to
        blip::Connection::State _connectionState;  // Current BLIP connection state

        Status                _pushStatus{};             // Current status of Pusher
        Status                _pullStatus{};             // Current status of Puller
        fleece::Stopwatch     _sinceDelegateCall;        // Time I last sent progress to the delegate
        ActivityLevel         _lastDelegateCallLevel{};  // Activity level I last reported to delegate
        bool                  _waitingToCallDelegate{};  // Is an async call to reportStatus pending?
        ReplicatedRevBatcher  _docsEnded;                // Recently-completed revs
        vector<SubReplicator> _subRepls;
        bool                  _getCollectionsRequested{};  // True while "getCollections" request pending
        alloc_slice           _remoteURL;
        bool                  _setMsgHandlerFor3_0_ClientDone{false};
        Retained<WeakHolder<blip::ConnectionDelegate>> _weakConnectionDelegateThis;
#ifdef LITECORE_CPPTEST
        // Used for testing purposes to delay the changes response to the remote
        bool _delayChangesResponse{false};

      public:
        bool _disableReplacementRevs{false};
#endif
    };
}  // namespace litecore::repl
