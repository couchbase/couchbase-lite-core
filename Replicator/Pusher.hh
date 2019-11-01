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
#include "Actor.hh"
#include "SequenceSet.hh"
#include "fleece/slice.hh"
#include "make_unique.h"
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace litecore { namespace repl {

    /** Top-level object managing the push side of replication (sending revisions.) */
    class Pusher : public Worker {
    public:
        Pusher(Replicator *replicator NONNULL);

        // Starts an active push
        void start(C4SequenceNumber sinceSequence)  {enqueue(&Pusher::_start, sinceSequence);}

        void checkpointIsInvalid() {
            _checkpointValid = false;
        }

        // Checks if a given document should be filtered by this pusher.  The revision body
        // must be loaded prior to this call.
        bool documentShouldBeFiltered(C4Document* doc) const {
            const auto body = doc->selectedRev.body;
            Assert(body.buf);
            const auto bodyContent = FLValue_FromData(body, kFLTrusted);
            return _options.pushFilter
                && _options.pushFilter(doc->docID, doc->selectedRev.flags, FLValue_AsDict(bodyContent), _options.callbackContext);
        }

        // Checks if a given sequence number is pending to be pushed
        bool isSequencePending(sequence_t seq) const {
            return _pendingSequences.contains(seq);
        }
        
    protected:
        virtual void afterEvent() override;

    private:
        void _start(C4SequenceNumber sinceSequence);
        bool passive() const                         {return _options.push <= kC4Passive;}
        virtual ActivityLevel computeActivityLevel() const override;
        void startSending(C4SequenceNumber sinceSequence);
        void handleSubChanges(Retained<blip::MessageIn> req);
        void gotChanges(std::shared_ptr<RevToSendList> changes, C4SequenceNumber lastSequence, C4Error err);
        void gotOutOfOrderChange(RevToSend* NONNULL);
        void sendChanges(std::shared_ptr<RevToSendList>);
        void maybeGetMoreChanges();
        void sendChangeList(RevToSendList);
        void maybeSendMoreRevs();
        void sendRevision(Retained<RevToSend>);
        void couldntSendRevision(RevToSend* NONNULL);
        void doneWithRev(RevToSend*, bool successful, bool pushed);
        void updateCheckpoint();
        void handleGetAttachment(Retained<MessageIn>);
        void handleProveAttachment(Retained<MessageIn>);
        void _attachmentSent();
        C4ReadStream* readBlobFromRequest(MessageIn *req NONNULL,
                                          slice &outDigest,
                                          Replicator::BlobProgress &outProgress,
                                          C4Error *outError);
        void filterByDocIDs(fleece::Array docIDs);

        using DocIDSet = std::shared_ptr<std::unordered_set<std::string>>;

        struct GetChangesParams {
            C4SequenceNumber since;
            DocIDSet docIDs;
            unsigned limit;
            bool continuous, getForeignAncestors;
            bool skipDeleted, skipForeign;
        };
        void getChanges(const GetChangesParams&);
        void dbChanged();
        bool shouldPushRev(RevToSend* NONNULL, C4DocEnumerator*, C4Database* NONNULL);
        void sendRevision(RevToSend *request NONNULL,
                          blip::MessageProgressCallback onProgress);
        alloc_slice createRevisionDelta(C4Document *doc NONNULL, RevToSend *request NONNULL,
                                        fleece::Dict root, size_t revSize,
                                        bool sendLegacyAttachments);
        fleece::slice getRevToSend(C4Document* NONNULL, const RevToSend&, C4Error *outError);

        static constexpr unsigned kDefaultChangeBatchSize = 200;  // # of changes to send in one msg
        static const unsigned kDefaultMaxHistory = 20;      // If "changes" response doesn't have one

        unsigned _changesBatchSize {kDefaultChangeBatchSize};   // # changes to get from db
        DocIDSet _docIDs;
        bool _continuous;
        bool _skipDeleted;
        bool _proposeChanges;
        bool _proposeChangesKnown;
        std::atomic<bool> _checkpointValid {true};

        C4SequenceNumber _lastSequence {0};       // Checkpointed last-sequence
        bool _gettingChanges {false};             // Waiting for _gotChanges() call?
        SequenceSet _pendingSequences;            // Sequences rcvd from db but not pushed yet
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

        C4BlobStore* _blobStore;
        c4::ref<C4DatabaseObserver> _changeObserver;        // Used in continuous push mode
        DocIDSet _pushDocIDs;                               // Optional set of doc IDs to push
        C4SequenceNumber _maxPushedSequence {0};            // Latest seq that's been pushed
        DocIDToRevMap _pushingDocs;                         // Revs being processed by push
        bool _getForeignAncestors {false};
        bool _skipForeignChanges {false};
    };
    
    
} }
