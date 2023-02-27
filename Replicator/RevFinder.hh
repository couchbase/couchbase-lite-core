//
// RevFinder.hh
//
//  Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Worker.hh"
#include "RemoteSequence.hh"
#include "ReplicatorTuning.hh"
#include "ReplicatorTypes.hh"
#include <deque>
#include <vector>

namespace litecore { namespace repl {

    /** Receives "changes" messages, and tells its delegate (the Puller) which revisions in them are new
        and should be pulled. */
    class RevFinder : public Worker {
      public:
        struct ChangeSequence {
            RemoteSequence sequence;
            uint64_t       bodySize{0};

            bool requested() const { return bodySize > 0; }
        };

        class Delegate : public Worker {
          public:
            Delegate(Worker *parent, const char *namePrefix, CollectionIndex coll) : Worker(parent, namePrefix, coll) {}

            virtual ~Delegate() = default;
            /** Tells the Delegate the peer has finished sending historical changes. */
            virtual void caughtUp() = 0;
            /** Tells the Delegate about the "rev" messages it will be receiving. */
            virtual void expectSequences(std::vector<ChangeSequence>) = 0;
            /** These document(s) are no longer accessible on the server and should be purged. */
            virtual void documentsRevoked(std::vector<Retained<RevToInsert>>) = 0;
        };

        RevFinder(Replicator *NONNULL, Delegate *NONNULL, CollectionIndex);

        /** Delegate must call this every time it receives a "rev" message. */
        void revReceived() { enqueue(FUNCTION_TO_QUEUE(RevFinder::_revReceived)); }

        /** Delegate calls this if it has to re-request a "rev" message, meaning that another call to
            revReceived() will be made in the future. */
        void reRequestingRev() { enqueue(FUNCTION_TO_QUEUE(RevFinder::_reRequestingRev)); }

        void onError(C4Error err) override;

        bool passive() const override { return _options->pull(collectionIndex()) <= kC4Passive; }

      private:
        static const size_t kMaxPossibleAncestors = 10;

        bool pullerHasCapacity() const { return _numRevsBeingRequested <= tuning::kMaxRevsBeingRequested; }

        void handleChanges(Retained<blip::MessageIn>);
        void handleMoreChanges();
        void handleChangesNow(blip::MessageIn *req);

        int  findRevs(fleece::Array, fleece::JSONEncoder &, std::vector<ChangeSequence> &);
        int  findProposedRevs(fleece::Array, fleece::JSONEncoder &, bool, std::vector<ChangeSequence> &);
        int  findProposedChange(slice docID, slice revID, slice parentRevID, alloc_slice &outCurrentRevID);
        void _revReceived();
        void _reRequestingRev();
        void checkDocAndRevID(slice docID, slice revID);

        Retained<Delegate>                    _delegate;
        std::deque<Retained<blip::MessageIn>> _waitingChangesMessages;  // Queued 'changes' messages
        unsigned _numRevsBeingRequested{0};      // # of 'rev' msgs requested but not yet received
        bool     _announcedDeltaSupport{false};  // Did I send "deltas:true" yet?
        bool     _mustBeProposed{false};         // Do I handle only "proposedChanges"?
    };

}}  // namespace litecore::repl
