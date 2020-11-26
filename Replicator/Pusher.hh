//
// Pusher.hh
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
#include "ChangesFeed.hh"
#include "Replicator.hh" // for BlobProgress
#include "ReplicatorTypes.hh"
#include "fleece/slice.hh"
#include <deque>
#include <unordered_map>

namespace litecore { namespace repl {

    /** Top-level object managing the push side of replication (sending revisions.) */
    class Pusher : public Worker, public ChangesFeed::Delegate {
    public:
        Pusher(Replicator *replicator NONNULL, Checkpointer&);

        // Starts an active push
        void start()  {enqueue(FUNCTION_TO_QUEUE(Pusher::_start));}

        // Called by Replicator when remote checkpoint doesn't match
        void checkpointIsInvalid()              {_changesFeed.setCheckpointValid(false);}

        // Called by the puller's RevFinder, via the Replicator
        void docRemoteAncestorChanged(alloc_slice docID, alloc_slice remoteAncestorRevID) {
            enqueue(FUNCTION_TO_QUEUE(Pusher::_docRemoteAncestorChanged), docID, remoteAncestorRevID);
        }

    protected:
        virtual void dbHasNewChanges() override {enqueue(FUNCTION_TO_QUEUE(Pusher::_dbHasNewChanges));}
        virtual void failedToGetChange(ReplicatedRev *rev, C4Error error, bool transient) override {
            finishedDocumentWithError(rev, error, transient);
        }
        virtual void afterEvent() override;
        virtual void _connectionClosed() override;
        virtual ActivityLevel computeActivityLevel() const override;

    private:
        void _start();
        bool isBusy() const;
        void startSending(C4SequenceNumber sinceSequence);
        void handleSubChanges(Retained<blip::MessageIn> req);
        void gotOutOfOrderChange(RevToSend* NONNULL);
        void sendChanges(RevToSendList&);
        void handleChangesResponse(RevToSendList&, blip::MessageIn*, bool proposedChanges);
        bool handleChangeResponse(RevToSend *change, Value response);
        bool handleProposedChangeResponse(RevToSend *change, Value response);
        void maybeGetMoreChanges()          {enqueue(FUNCTION_TO_QUEUE(Pusher::_maybeGetMoreChanges));}
        void _maybeGetMoreChanges();
        void gotChanges(ChangesFeed::Changes);
        void _dbHasNewChanges();
        void sendChangeList(RevToSendList);
        bool shouldRetryConflictWithNewerAncestor(RevToSend* NONNULL);
        void _docRemoteAncestorChanged(alloc_slice docID, alloc_slice remoteAncestorRevID);
        bool getForeignAncestors() const    {return _proposeChanges || !_proposeChangesKnown;}

        // Pusher+Attachments.cc:
        void handleGetAttachment(Retained<blip::MessageIn>);
        void handleProveAttachment(Retained<blip::MessageIn>);
        void _attachmentSent();
        C4ReadStream* readBlobFromRequest(blip::MessageIn *req NONNULL,
                                          slice &outDigest,
                                          Replicator::BlobProgress &outProgress,
                                          C4Error *outError);
        // Pusher+Revs.cc:
        void maybeSendMoreRevs();
        void retryRevs(RevToSendList, bool immediate);
        void sendRevision(Retained<RevToSend>);
        void onRevProgress(Retained<RevToSend> rev, const blip::MessageProgress&);
        void couldntSendRevision(RevToSend* NONNULL);
        void doneWithRev(RevToSend*, bool successful, bool pushed);
        alloc_slice createRevisionDelta(C4Document *doc NONNULL, RevToSend *request NONNULL,
                                        fleece::Dict root, size_t revSize,
                                        bool sendLegacyAttachments);
        void revToSendIsObsolete(const RevToSend &request, C4Error *c4err);

        using DocIDToRevMap = std::unordered_map<alloc_slice, Retained<RevToSend>>;

        bool _continuous;
        bool _proposeChanges;
        bool _proposeChangesKnown;
        ReplicatorChangesFeed _changesFeed;
        DocIDToRevMap _pushingDocs;               // Revs being processed by push
        DocIDToRevMap _conflictsIMightRetry;
        C4SequenceNumber _lastSequenceRead {0};   // Last sequence read from db
        C4SequenceNumber _lastSequenceLogged {0}; // Checkpointed last-sequence
        Checkpointer& _checkpointer;              // Tracks checkpoints & pending sequences
        bool _started {false};
        bool _caughtUp {false};                   // Received backlog of pre-existing changes?
        bool _continuousCaughtUp {true};          // Caught up with change notifications?
        bool _deltasOK {false};                   // OK to send revs in delta form?
        unsigned _changeListsInFlight {0};        // # change lists being requested from db or sent to peer
        unsigned _revisionsInFlight {0};          // # 'rev' messages being sent
        blip::MessageSize _revisionBytesAwaitingReply {0}; // # 'rev' message bytes sent but not replied
        unsigned _blobsInFlight {0};              // # of blobs being sent
        std::deque<Retained<RevToSend>> _revQueue;// Revs to send to peer but not sent yet
        RevToSendList _revsToRetry;               // Revs that failed with a transient error
    };
    
    
} }
