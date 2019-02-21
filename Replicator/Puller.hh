//
// Puller.hh
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
#include "Actor.hh"
#include "RemoteSequenceSet.hh"
#include "Batcher.hh"
#include <deque>

namespace litecore { namespace repl {
    class IncomingRev;
    class RevToInsert;

    class Puller : public Worker {
    public:
        Puller(Replicator*);

        void setSkipDeleted()                   {enqueue(&Puller::_setSkipDeleted);}

        // Starts an active pull
        void start(alloc_slice sinceSequence)   {enqueue(&Puller::_start, sinceSequence);}

        // Called only by IncomingRev
        void revWasHandled(IncomingRev *inc);

        void insertRevision(RevToInsert *rev);

    protected:
        virtual std::string loggingClassName() const override  {
            return _options.pull >= kC4OneShot ? "Pull" : "pull";
        }

        bool nonPassive() const                 {return _options.pull > kC4Passive;}
        virtual void _childChangedStatus(Worker *task, Status) override;
        virtual ActivityLevel computeActivityLevel() const override;
        void activityLevelChanged(ActivityLevel level);

    private:
        Replicator* replicator() const          {return (Replicator*)_parent.get();}
        void _start(alloc_slice sinceSequence);
        void handleChanges(Retained<MessageIn>);
        void handleMoreChanges();
        void handleChangesNow(Retained<MessageIn> req);
        void handleRev(Retained<MessageIn>);
        void handleNoRev(Retained<MessageIn>);
        void startIncomingRev(MessageIn*);
        void _revsFinished();
        void completedSequence(alloc_slice sequence, bool withTransientError =false);

        void _setSkipDeleted()                  {_skipDeleted = true;}

        std::vector<bool> findOrRequestRevs(Retained<blip::MessageIn> req);

        bool findAncestors(slice docID, slice revID,
                           std::vector<alloc_slice> &ancestors);
        int findProposedChange(slice docID, slice revID, slice parentRevID,
                               alloc_slice &outCurrentRevID);
        void updateRemoteRev(C4Document* NONNULL);
        void _insertRevisionsNow();

        static const size_t kMaxPossibleAncestors = 10;

        alloc_slice _lastSequence;          // Checkpointed sequence
        bool _skipDeleted {false};          // Don't pull deleted docs (on 1st pull)
        bool _caughtUp {false};             // Got all historic sequences, now up to date
        bool _fatalError {false};           // Have I gotten a fatal error?
        bool _disableDeltaSupport;

        RemoteSequenceSet _missingSequences; // Known sequences I need to pull
        std::deque<Retained<MessageIn>> _waitingChangesMessages; // Queued 'changes' messages
        std::deque<Retained<MessageIn>> _waitingRevMessages;     // Queued 'rev' messages
        std::vector<Retained<IncomingRev>> _spareIncomingRevs;   // Cache of IncomingRev objects
        actor::ActorBatcher<Puller,IncomingRev> _returningRevs;
        unsigned _pendingRevMessages {0};   // # of 'rev' msgs expected but not yet being processed
        unsigned _activeIncomingRevs {0};   // # of IncomingRev workers running

        actor::ActorBatcher<Puller,RevToInsert> _revsToInsert; // Pending revs to be added to db
        bool _insertionScheduled {false};                   // True if call to insert/sync pending
        std::mutex _insertionQueueMutex;                    // For safe access to the above
        bool _disableBlobSupport {false};                   // for testing only
        bool _announcedDeltaSupport {false};                // Did I send "deltas:true" yet?

#if __APPLE__
        // This helps limit the number of threads used by GCD:
        virtual actor::Mailbox* mailboxForChildren() override       {return &_revMailbox;}
        actor::Mailbox _revMailbox;
#endif
    };


} }
