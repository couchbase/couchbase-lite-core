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
#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "ReplicatorTypes.hh"
#include "access_lock.hh"
#include "Actor.hh"
#include "SequenceSet.hh"
#include "fleece/slice.hh"
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace litecore { namespace repl {

    /** Top-level object managing the push side of replication (sending revisions.) */
    class Pusher : public Worker {
    public:
        Pusher(Replicator *replicator NONNULL, Checkpointer&);

        // Starts an active push
        void start()  {enqueue(&Pusher::_start);}

        void checkpointIsInvalid() {
            _checkpointValid = false;
        }
        
        void docRemoteAncestorChanged(alloc_slice docID, alloc_slice remoteAncestorRevID) {
            enqueue(&Pusher::_docRemoteAncestorChanged, docID, remoteAncestorRevID);
        }

        void onError(C4Error err) override;

    protected:
        virtual void afterEvent() override;
        virtual void _connectionClosed() override;

    private:
        void _start();
        virtual ActivityLevel computeActivityLevel() const override;
        void startSending(C4SequenceNumber sinceSequence);
        void handleSubChanges(Retained<blip::MessageIn> req);
        void gotChanges(std::shared_ptr<RevToSendList> changes, C4SequenceNumber lastSequence, C4Error err);
        void gotOutOfOrderChange(RevToSend* NONNULL);
        void sendChanges(std::shared_ptr<RevToSendList>);
        void maybeGetMoreChanges();
        void sendChangeList(RevToSendList);
        void maybeSendMoreRevs();
        void retryRevs(RevToSendList, bool immediate);
        void sendRevision(Retained<RevToSend>);
        void couldntSendRevision(RevToSend* NONNULL);
        void doneWithRev(RevToSend*, bool successful, bool pushed);
        void handleGetAttachment(Retained<MessageIn>);
        void handleProveAttachment(Retained<MessageIn>);
        void _attachmentSent();
        C4ReadStream* readBlobFromRequest(MessageIn *req NONNULL,
                                          slice &outDigest,
                                          Replicator::BlobProgress &outProgress,
                                          C4Error *outError);
        void filterByDocIDs(fleece::Array docIDs);

        using DocIDSet = std::shared_ptr<std::unordered_set<std::string>>;

        void getMoreChanges();
        void getObservedChanges();
        void dbChanged();
        Retained<RevToSend> revToSend(C4DocumentInfo&, C4DocEnumerator*, C4Database* NONNULL);
        bool shouldPushRev(Retained<RevToSend>, C4DocEnumerator*, C4Database* NONNULL);
        void sendRevision(RevToSend *request NONNULL,
                          blip::MessageProgressCallback onProgress);
        alloc_slice createRevisionDelta(C4Document *doc NONNULL, RevToSend *request NONNULL,
                                        fleece::Dict root, size_t revSize,
                                        bool sendLegacyAttachments);
        fleece::slice getRevToSend(C4Document* NONNULL, const RevToSend&, C4Error *outError);
        bool getRemoteRevID(RevToSend *rev, C4Document *doc);
        void revToSendIsObsolete(const RevToSend &request, C4Error *c4err);
        bool shouldRetryConflictWithNewerAncestor(RevToSend* NONNULL);
        void _docRemoteAncestorChanged(alloc_slice docID, alloc_slice remoteAncestorRevID);
        bool isBusy() const;

        bool getForeignAncestors() const    {return _proposeChanges || !_proposeChangesKnown;}

        static constexpr unsigned kDefaultChangeBatchSize = 200;  // # of changes to send in one msg
        static const unsigned kDefaultMaxHistory = 20;      // If "changes" response doesn't have one

        unsigned _changesBatchSize {kDefaultChangeBatchSize};   // # changes to get from db
        DocIDSet _docIDs;
        bool _continuous;
        bool _skipDeleted;
        bool _proposeChanges;
        bool _proposeChangesKnown;
        std::atomic<bool> _checkpointValid {true};

        C4SequenceNumber _lastSequenceLogged {0}; // Checkpointed last-sequence
        bool _gettingChanges {false};             // Waiting for _gotChanges() call?
        Checkpointer& _checkpointer;              // Tracks checkpoints & pending sequences
        C4SequenceNumber _lastSequenceRead {0};   // Last sequence read from db
        bool _started {false};
        bool _caughtUp {false};                   // Received backlog of existing changes?
        bool _deltasOK {false};                   // OK to send revs in delta form?
        unsigned _changeListsInFlight {0};        // # change lists being requested from db or sent to peer
        unsigned _revisionsInFlight {0};          // # 'rev' messages being sent
        MessageSize _revisionBytesAwaitingReply {0}; // # 'rev' message bytes sent but not replied
        unsigned _blobsInFlight {0};              // # of blobs being sent
        std::deque<Retained<RevToSend>> _revsToSend;  // Revs to send to peer but not sent yet
        RevToSendList _revsToRetry;                     // Revs that failed with a transient error

        using DocIDToRevMap = std::unordered_map<alloc_slice, Retained<RevToSend>, fleece::sliceHash>;

        c4::ref<C4DatabaseObserver> _changeObserver;        // Used in continuous push mode
        C4SequenceNumber _maxPushedSequence {0};            // Latest seq that's been pushed
        DocIDToRevMap _pushingDocs;                         // Revs being processed by push
        DocIDToRevMap _conflictsIMightRetry;
        bool _waitingForObservedChanges {false};
    };
    
    
} }
