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
#include "Worker.hh"
#include "RevFinder.hh"
#include "ReplicatorTypes.hh"
#include "RemoteSequenceSet.hh"
#include "Batcher.hh"
#include <deque>
#include <vector>

namespace litecore { namespace repl {
    class IncomingRev;
    class RevToInsert;
    class Inserter;

    /** Top-level object managing the pull side of replication (receiving revisions.) */
    class Puller final : public RevFinder::Delegate {
    public:
        Puller(Replicator* NONNULL);

        void setSkipDeleted()                       {_skipDeleted = true;}

        // Starts an active pull
        void start(RemoteSequence sinceSequence)    {enqueue(FUNCTION_TO_QUEUE(Puller::_start), sinceSequence);}

        // Called only by IncomingRev
        void revWasProvisionallyHandled()           {_provisionallyHandledRevs.add(1);}
        void revWasHandled(IncomingRev *inc NONNULL);
        void revReRequested(fleece::Retained<IncomingRev> inc);

        void insertRevision(RevToInsert *rev NONNULL);

        int progressNotificationLevel() const override;

    protected:
        virtual void caughtUp() override        {enqueue(FUNCTION_TO_QUEUE(Puller::_setCaughtUp));}
        virtual void expectSequences(std::vector<RevFinder::ChangeSequence> changes) override {
            enqueue(FUNCTION_TO_QUEUE(Puller::_expectSequences), move(changes));
        }
        virtual void documentsRevoked(std::vector<Retained<RevToInsert>> revs) override {
            enqueue(FUNCTION_TO_QUEUE(Puller::_documentsRevoked), move(revs));
        }
        virtual void _childChangedStatus(Worker *task NONNULL, Status) override;
        virtual ActivityLevel computeActivityLevel() const override;
        void activityLevelChanged(ActivityLevel level);

    private:
        void _start(RemoteSequence sinceSequence);
        void _expectSequences(std::vector<RevFinder::ChangeSequence>);
        void _documentsRevoked(std::vector<Retained<RevToInsert>>);
        void handleRev(Retained<blip::MessageIn>);
        void handleNoRev(Retained<blip::MessageIn>);
        Retained<IncomingRev> makeIncomingRev();
        void startIncomingRev(blip::MessageIn* NONNULL);
        void maybeStartIncomingRevs();
        void _revsWereProvisionallyHandled();
        void _revsFinished(int gen);
        void _revReRequested(uint64_t missingBodySize);
        void completedSequence(const RemoteSequence&,
                               bool withTransientError =false, bool updateCheckpoint =true);
        void updateLastSequence();

        void _setCaughtUp()                     {_caughtUp = true;}

        void updateRemoteRev(C4Document* NONNULL);

        RemoteSequence _lastSequence;       // Checkpointed sequence
        bool _skipDeleted {false};          // Don't pull deleted docs (on 1st pull)
        bool _caughtUp {false};             // Got all historic sequences, now up to date
        bool _fatalError {false};           // Have I gotten a fatal error?

        RemoteSequenceSet _missingSequences; // Known sequences I need to pull
        std::deque<Retained<blip::MessageIn>> _waitingRevMessages;     // Queued 'rev' messages
        mutable std::vector<Retained<IncomingRev>> _spareIncomingRevs;   // Cache of IncomingRevs
        actor::ActorCountBatcher<Puller> _provisionallyHandledRevs;
        actor::ActorBatcher<Puller,IncomingRev> _returningRevs;
        Retained<Inserter> _inserter;
        mutable Retained<RevFinder> _revFinder;
        unsigned _pendingRevMessages {0};   // # of 'rev' msgs expected but not yet being processed
        unsigned _activeIncomingRevs {0};   // # of IncomingRev workers running
        unsigned _unfinishedIncomingRevs {0};

#if __APPLE__
        // This helps limit the number of threads used by GCD:
        virtual actor::Mailbox* mailboxForChildren() override       {return &_revMailbox;}
        actor::Mailbox _revMailbox;
#endif
    };


} }
