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
#include "DBWorker.hh"
#include "Actor.hh"
#include "SequenceSet.hh"
#include "fleece/slice.hh"
#include "make_unique.h"
#include <deque>
#include <unordered_map>

namespace litecore { namespace repl {

    class Pusher : public Worker {
    public:
        Pusher(Replicator *replicator, DBWorker *dbActor);

        // Starts an active push
        void start(C4SequenceNumber sinceSequence)  {enqueue(&Pusher::_start, sinceSequence);}

        // Sent by Replicator in response to dbGetChanges
        void gotChanges(std::shared_ptr<RevToSendList> changes,
                        C4SequenceNumber lastSequence,
                        C4Error err)
        {
            enqueue(&Pusher::_gotChanges, move(changes), lastSequence, err);
        }

        void couldntSendRevision(RevToSend* req) {
            enqueue(&Pusher::_couldntSendRevision, retained(req));
        }

    protected:
        virtual std::string loggingClassName() const override {return "Push";}

    private:
        Replicator* replicator() const                  {return (Replicator*)_parent.get();}
        void _start(C4SequenceNumber sinceSequence);
        bool passive() const                         {return _options.push <= kC4Passive;}
        virtual ActivityLevel computeActivityLevel() const override;
        void startSending(C4SequenceNumber sinceSequence);
        void handleSubChanges(Retained<blip::MessageIn> req);
        void _gotChanges(std::shared_ptr<RevToSendList> changes, C4SequenceNumber lastSequence, C4Error err);
        void sendChanges(std::shared_ptr<RevToSendList>);
        void maybeGetMoreChanges();
        void sendChangeList(RevToSendList);
        void maybeSendMoreRevs();
        void sendRevision(Retained<RevToSend>);
        void _couldntSendRevision(Retained<RevToSend>);
        void doneWithRev(const RevToSend*, bool successful);
        void updateCheckpoint();
        void handleGetAttachment(Retained<MessageIn>);
        void handleProveAttachment(Retained<MessageIn>);
        void _attachmentSent();

        C4ReadStream* readBlobFromRequest(MessageIn *req,
                                          slice &outDigest,
                                          Replicator::BlobProgress &outProgress,
                                          C4Error *outError);
        void filterByDocIDs(fleece::Array docIDs);

        static constexpr unsigned kDefaultChangeBatchSize = 200;  // # of changes to send in one msg
        static const unsigned kDefaultMaxHistory = 20;      // If "changes" response doesn't have one

        Retained<DBWorker> _dbWorker;
        unsigned _changesBatchSize {kDefaultChangeBatchSize};   // # changes to get from db
        DocIDSet _docIDs;
        bool _continuous;
        bool _skipDeleted;
        bool _proposeChanges;
        bool _proposeChangesKnown;

        C4SequenceNumber _lastSequence {0};       // Checkpointed last-sequence
        bool _gettingChanges {false};             // Waiting for _gotChanges() call?
        SequenceSet _pendingSequences;            // Sequences rcvd from db but not pushed yet
        C4SequenceNumber _lastSequenceRead {0};   // Last sequence read from db
        bool _started {false};
        bool _caughtUp {false};                   // Received backlog of existing changes?
        unsigned _changeListsInFlight {0};        // # change lists being requested from db or sent to peer
        unsigned _revisionsInFlight {0};          // # 'rev' messages being sent
        MessageSize _revisionBytesAwaitingReply {0}; // # 'rev' message bytes sent but not replied
        unsigned _blobsInFlight {0};              // # of blobs being sent
        std::deque<Retained<RevToSend>> _revsToSend;  // Revs to send to peer but not sent yet
        std::unordered_map<alloc_slice, Retained<RevToSend>, fleece::sliceHash> _activeDocs;
    };
    
    
} }
