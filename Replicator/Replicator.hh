//
// Replicator.hh
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
#include "Worker.hh"
#include "Checkpointer.hh"
#include "ReplicatedRev.hh"
#include "BLIPConnection.hh"
#include "Batcher.hh"
#include "fleece/Fleece.hh"
#include "InstanceCounted.hh"
#include "Stopwatch.hh"
#include "Certificate.hh"
#include <unordered_set>

using namespace litecore::blip;

struct C4UUID;

namespace litecore { namespace repl {

    class Pusher;
    class Puller;


    static constexpr const char *kReplicatorProtocolName = "+CBMobile_2";


    /** The top-level replicator object, which runs the BLIP connection.
        Pull and push operations are run by subidiary Puller and Pusher objects.
        The database will only be accessed by the DBAgent object. */
    class Replicator : public Worker, ConnectionDelegate, public InstanceCountedIn<Replicator> {
    public:

        class Delegate;
        using CloseStatus = blip::Connection::CloseStatus;
        using Options = litecore::repl::Options;

        Replicator(C4Database*,
                   websocket::WebSocket*,
                   Delegate&,
                   Options);

        struct BlobProgress {
            Dir         dir;
            alloc_slice docID;
            alloc_slice docProperty;
            C4BlobKey   key;
            uint64_t    bytesCompleted;
            uint64_t    bytesTotal;
            C4Error     error;
        };

        using DocumentsEnded = std::vector<Retained<ReplicatedRev>>;

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
        void stop()                             {enqueue(&Replicator::_stop);}

        /** Tears down a Replicator state including any reference cycles.
            The Replicator must have either already stopped, or never started.
            No further delegate callbacks will be made!  */
        void terminate();

        /** Invokes the callback for each document which has revisions pending push */
        bool pendingDocumentIDs(Checkpointer::PendingDocCallback, C4Error* outErr);

        /** Checks if the document with the given ID has any pending revisions to push.  If unable, returns an empty optional. */
        std::optional<bool> isDocumentPending(slice docId, C4Error* outErr);

        // exposed for unit tests:
        websocket::WebSocket* webSocket() const {return connection().webSocket();}
        
        Checkpointer& checkpointer()            {return _checkpointer;}

        void endedDocument(ReplicatedRev *d NONNULL);
        void onBlobProgress(const BlobProgress &progress) {
            enqueue(&Replicator::_onBlobProgress, progress);
        }
        
        void docRemoteAncestorChanged(alloc_slice docID, alloc_slice revID);

        Retained<Replicator> replicatorIfAny() override         {return this;}

    protected:
        virtual std::string loggingClassName() const override  {
            return _options.pull >= kC4OneShot || _options.push >= kC4OneShot ? "Repl" : "repl";
        }

        // BLIP ConnectionDelegate API:
        virtual void onHTTPResponse(int status, const websocket::Headers &headers) override;
        virtual void onTLSCertificate(slice certData) override;
        virtual void onConnect() override
                                                {enqueue(&Replicator::_onConnect);}
        virtual void onClose(Connection::CloseStatus status, Connection::State state) override
                                                {enqueue(&Replicator::_onClose, status, state);}
        virtual void onRequestReceived(blip::MessageIn *msg) override
                                        {enqueue(&Replicator::_onRequestReceived, retained(msg));}
        virtual void changedStatus() override;

        virtual void onError(C4Error error) override;

    private:
        void _onHTTPResponse(int status, websocket::Headers headers);
        void _onConnect();
        void _onError(int errcode, fleece::alloc_slice reason);
        void _onClose(Connection::CloseStatus, Connection::State);
        void _onRequestReceived(Retained<blip::MessageIn> msg);

        void _start(bool reset);
        void _stop();
        void _disconnect(websocket::CloseCode closeCode, slice message);
        void _findExistingConflicts();        
        bool getLocalCheckpoint(bool reset);
        void getRemoteCheckpoint(bool refresh);
        void startReplicating();
        virtual ActivityLevel computeActivityLevel() const override;
        void reportStatus();

        void updateCheckpoint();
        void saveCheckpoint(alloc_slice json)       {enqueue(&Replicator::_saveCheckpoint, json);}
        void _saveCheckpoint(alloc_slice json);
        void saveCheckpointNow();

        virtual void _childChangedStatus(Worker *task, Status taskStatus) override;
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
        
        CloseStatus _closeStatus;
        Delegate* _delegate;
        Retained<Pusher> _pusher;
        Retained<Puller> _puller;
        Connection::State _connectionState;
        Status _pushStatus {}, _pullStatus {};
        fleece::Stopwatch _sinceDelegateCall;
        ActivityLevel _lastDelegateCallLevel {};
        bool _waitingToCallDelegate {false};
        actor::ActorBatcher<Replicator, ReplicatedRev> _docsEnded;

        Checkpointer _checkpointer;
        bool _hadLocalCheckpoint {false};
        bool _remoteCheckpointRequested {false};
        bool _remoteCheckpointReceived {false};
        alloc_slice _checkpointJSONToSave;
        alloc_slice _remoteCheckpointDocID;
        alloc_slice _remoteCheckpointRevID;
    };

} }
