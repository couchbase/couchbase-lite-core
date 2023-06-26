//
// Puller.hh
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
#include "RevFinder.hh"
#include "ReplicatorTypes.hh"
#include "RemoteSequenceSet.hh"
#include "Batcher.hh"
#include <deque>
#include <utility>
#include <vector>

namespace litecore::repl {
    class IncomingRev;
    class RevToInsert;
    class Inserter;

    /** Top-level object managing the pull side of replication (receiving revisions.) */
    class Puller final : public RevFinder::Delegate {
      public:
        Puller(Replicator* NONNULL, CollectionIndex);

        void setSkipDeleted() { _skipDeleted = true; }

        // Starts an active pull
        void start(RemoteSequence sinceSequence) {
            enqueue(FUNCTION_TO_QUEUE(Puller::_start), std::move(sinceSequence));
        }

        // Called only by IncomingRev
        void revWasProvisionallyHandled() { _provisionallyHandledRevs.add(1); }

        void revWasHandled(IncomingRev* inc NONNULL);
        void revReRequested(uint64_t missingBodySize);

        void insertRevision(RevToInsert* rev NONNULL);

        bool passive() const override { return _options->pull(collectionIndex()) <= kC4Passive; }

      protected:
        void caughtUp() override { enqueue(FUNCTION_TO_QUEUE(Puller::_setCaughtUp)); }

        void expectSequences(std::vector<RevFinder::ChangeSequence> changes) override {
            enqueue(FUNCTION_TO_QUEUE(Puller::_expectSequences), std::move(changes));
        }

        void documentsRevoked(std::vector<Retained<RevToInsert>> revs) override {
            enqueue(FUNCTION_TO_QUEUE(Puller::_documentsRevoked), std::move(revs));
        }

        void          _childChangedStatus(Retained<Worker>, Status) override;
        ActivityLevel computeActivityLevel() const override;
        void          activityLevelChanged(ActivityLevel level);

      private:
        void                  _start(RemoteSequence sinceSequence);
        void                  _expectSequences(std::vector<RevFinder::ChangeSequence>);
        void                  _documentsRevoked(std::vector<Retained<RevToInsert>>);
        void                  handleRev(Retained<blip::MessageIn>);
        void                  handleNoRev(Retained<blip::MessageIn>);
        Retained<IncomingRev> makeIncomingRev();
        void                  startIncomingRev(blip::MessageIn* NONNULL);
        void                  maybeStartIncomingRevs();
        void                  _revsWereProvisionallyHandled();
        void                  _revsFinished(int gen);
        void                  _revReRequested(uint64_t missingBodySize);
        void completedSequence(const RemoteSequence&, bool withTransientError = false, bool updateCheckpoint = true);
        void updateLastSequence();

        void _setCaughtUp() { _caughtUp = true; }

        void updateRemoteRev(C4Document* NONNULL);

        RemoteSequence _lastSequence;        // Checkpointed sequence
        bool           _skipDeleted{false};  // Don't pull deleted docs (on 1st pull)
        bool           _caughtUp{false};     // Got all historic sequences, now up to date
        bool           _fatalError{false};   // Have I gotten a fatal error?

        RemoteSequenceSet                          _missingSequences;    // Known sequences I need to pull
        std::deque<Retained<blip::MessageIn>>      _waitingRevMessages;  // Queued 'rev' messages
        mutable std::vector<Retained<IncomingRev>> _spareIncomingRevs;   // Cache of IncomingRevs
        actor::ActorCountBatcher<Puller>           _provisionallyHandledRevs;
        actor::ActorBatcher<Puller, IncomingRev>   _returningRevs;
#if __APPLE__
        // This helps limit the number of threads used by GCD:
        actor::Mailbox* mailboxForChildren() override { return &_revMailbox; }

        // This field must go before _revFinder because "this" is passed in "new RevFinder(replicator, this)," which will
        // call this->mailboxForChildren() which depends on it.
        actor::Mailbox _revMailbox;
#endif
        Retained<Inserter>          _inserter;
        mutable Retained<RevFinder> _revFinder;
        unsigned                    _pendingRevMessages{0};  // # of 'rev' msgs expected but not yet being processed
        unsigned                    _activeIncomingRevs{0};  // # of IncomingRev workers running
        unsigned                    _unfinishedIncomingRevs{0};
    };


}  // namespace litecore::repl
