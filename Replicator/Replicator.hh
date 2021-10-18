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
#include "fleece/Fleece.hh"
#include "InstanceCounted.hh"
#include "Stopwatch.hh"
#include <array>

namespace litecore { namespace repl {

    class Pusher;
    class Puller;
    class ReplicatedRev;

    static const array<string, 2> kCompatProtocols = {{
        string(blip::Connection::kWSProtocolName) + "+CBMobile_3",
        string(blip::Connection::kWSProtocolName) + "+CBMobile_2"
    }};


    /** The top-level replicator object, which runs the BLIP connection.
        Pull and push operations are run by subidiary Puller and Pusher objects.
        The database will only be accessed by the DBAgent object. */
    class Replicator final : public Worker,
                             private blip::ConnectionDelegate,
                             public InstanceCountedIn<Replicator> {
    public:

        class Delegate;
        using CloseStatus = blip::Connection::CloseStatus;
        using Options = litecore::repl::Options;

        Replicator(C4Database* NONNULL,
                   websocket::WebSocket* NONNULL,
                   Delegate&,
                   Options);

        struct BlobProgress {
            Dir         dir;
            alloc_slice collectionName;
            alloc_slice docID;
            alloc_slice docProperty;
            C4BlobKey   key;
            uint64_t    bytesCompleted;
            uint64_t    bytesTotal;
            C4Error     error;
        };

        using DocumentsEnded = std::vector<Retained<ReplicatedRev>>;
                                 
        static std::string ProtocolName();

        /** Replicator delegate; receives progress & error notifications. */
        class Delegate {
        public:
            virtual ~Delegate() =default;

            virtual void replicatorGotHTTPResponse(Replicator* NONNULL,
                                                   int status,
                                                   const websocket::Headers &headers) { }
            virtual void replicatorGotTLSCertificate(slice certData) =0;
            virtual void replicatorStatusChanged(Replicator* NONNULL,
                                                 const Status&) =0;
            virtual void replicatorConnectionClosed(Replicator* NONNULL,
                                                    const CloseStatus&)  { }
            virtual void replicatorDocumentsEnded(Replicator* NONNULL,
                                                  const DocumentsEnded&) =0;
            virtual void replicatorBlobProgress(Replicator* NONNULL,
                                                const BlobProgress&) = 0;
        };

        Status status() const                   {return Worker::status();}   //FIX: Needs to be thread-safe

        void start(bool reset = false, bool synchronous =false); 
        void stop()                             {enqueue(FUNCTION_TO_QUEUE(Replicator::_stop));}

        /** Tears down a Replicator state including any reference cycles.
            The Replicator must have either already stopped, or never started.
            No further delegate callbacks will be made!  */
        void terminate();

        /** Invokes the callback for each document which has revisions pending push */
        void pendingDocumentIDs(Checkpointer::PendingDocCallback);

        /** Checks if the document with the given ID has any pending revisions to push */
        bool isDocumentPending(slice docID);

        Checkpointer& checkpointer()            {return _checkpointer;}

        void endedDocument(ReplicatedRev *d NONNULL);
        void onBlobProgress(const BlobProgress &progress) {
            enqueue(FUNCTION_TO_QUEUE(Replicator::_onBlobProgress), progress);
        }
        
        void docRemoteAncestorChanged(alloc_slice docID, alloc_slice revID);

        Retained<Replicator> replicatorIfAny() override         {return this;}

        // exposed for unit tests:
        websocket::WebSocket* webSocket() const {return connection().webSocket();}

    protected:
        virtual std::string loggingClassName() const override  {
            return _options.pull >= kC4OneShot || _options.push >= kC4OneShot ? "Repl" : "repl";
        }

        // BLIP ConnectionDelegate API:
        virtual void onHTTPResponse(int status, const websocket::Headers &headers) override;
        virtual void onTLSCertificate(slice certData) override;
        virtual void onConnect() override
                                                {enqueue(FUNCTION_TO_QUEUE(Replicator::_onConnect));}
        virtual void onClose(CloseStatus status, blip::Connection::State state) override
                                                {enqueue(FUNCTION_TO_QUEUE(Replicator::_onClose), status, state);}
        virtual void onRequestReceived(blip::MessageIn *msg NONNULL) override
                                        {enqueue(FUNCTION_TO_QUEUE(Replicator::_onRequestReceived), retained(msg));}
        virtual void changedStatus() override;

        virtual void onError(C4Error error) override;

        // Worker method overrides:
        virtual ActivityLevel computeActivityLevel() const override;
        virtual void _childChangedStatus(Worker *task, Status taskStatus) override;

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
        bool getLocalCheckpoint(bool reset);
        void getRemoteCheckpoint(bool refresh);
        void startReplicating();
        void reportStatus();

        void updateCheckpoint();
        void saveCheckpoint(alloc_slice json)       {enqueue(FUNCTION_TO_QUEUE(Replicator::_saveCheckpoint), json);}
        void _saveCheckpoint(alloc_slice json);
        void saveCheckpointNow();

        void notifyEndedDocuments(int gen =actor::AnyGen);
        void _onBlobProgress(BlobProgress);

        // Checkpoints:
        void checkpointIsInvalid();
        std::string remoteDBIDString() const;
        void handleGetCheckpoint(Retained<blip::MessageIn>);
        void handleSetCheckpoint(Retained<blip::MessageIn>);
        void returnForbidden(Retained<blip::MessageIn>);
        slice getPeerCheckpointDocID(blip::MessageIn* request, const char *whatFor) const;

        // Member variables:

        using ReplicatedRevBatcher = actor::ActorBatcher<Replicator, ReplicatedRev>;
        
        Delegate*         _delegate;                   // Delegate whom I report progress/errors to
        Retained<Pusher>  _pusher;                     // Object that manages outgoing revs
        Retained<Puller>  _puller;                     // Object that manages incoming revs
        blip::Connection::State _connectionState;      // Current BLIP connection state

        Status            _pushStatus {};              // Current status of Pusher
        Status            _pullStatus {};              // Current status of Puller
        fleece::Stopwatch _sinceDelegateCall;          // Time I last sent progress to the delegate
        ActivityLevel     _lastDelegateCallLevel {};   // Activity level I last reported to delegate
        bool              _waitingToCallDelegate {};   // Is an async call to reportStatus pending?
        ReplicatedRevBatcher _docsEnded;               // Recently-completed revs

        Checkpointer      _checkpointer;               // Object that manages checkpoints
        bool              _hadLocalCheckpoint {};      // True if local checkpoint pre-existed
        bool              _remoteCheckpointRequested{};// True while "getCheckpoint" request pending
        bool              _remoteCheckpointReceived {};// True if I got a "getCheckpoint" response
        alloc_slice       _checkpointJSONToSave;       // JSON waiting to be saved to the checkpts
        alloc_slice       _remoteCheckpointDocID;      // Checkpoint docID to use with peer
        alloc_slice       _remoteCheckpointRevID;      // Latest revID of remote checkpoint
    };

} }
