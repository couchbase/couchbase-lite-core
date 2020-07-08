//
// RevFinder.hh
//
//  Copyright (c) 2019 Couchbase. All rights reserved.
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
#include <deque>

namespace litecore { namespace repl {

    /** Receives "changes" messages, and tells its delegate (the Puller) which revisions in them are new
        and should be pulled. */
    class RevFinder : public Worker {
    public:
        struct ChangeSequence {
            RemoteSequence  sequence;
            uint64_t        bodySize {0};

            bool requested() const {return bodySize > 0;}
        };

        class Delegate {
        public:
            virtual ~Delegate() { }
            /** Tells the Delegate the peer has finished sending historical changes. */
            virtual void caughtUp() =0;
            /** Tells the Delegate about the "rev" messages it will be receiving. */
            virtual void expectSequences(std::vector<ChangeSequence>) =0;
        };

        RevFinder(Replicator* NONNULL, Delegate&);

        /** Delegate must call this every time it receives a "rev" message. */
        void revReceived()     {enqueue(&RevFinder::_revReceived);}

    private:
        static const size_t kMaxPossibleAncestors = 10;

        bool pullerHasCapacity() const   {return _pendingRevMessages <= tuning::kMaxPendingRevs;}
        void handleChanges(Retained<MessageIn>);
        void handleMoreChanges();
        void handleChangesNow(MessageIn *req);

        void findOrRequestRevs(Retained<blip::MessageIn>);
        unsigned findRevs(fleece::Array, fleece::Encoder&, std::vector<ChangeSequence>&);
        unsigned findProposedRevs(fleece::Array, fleece::Encoder&, std::vector<ChangeSequence>&);
        int findProposedChange(slice docID, slice revID, slice parentRevID,
                               alloc_slice &outCurrentRevID);
        void _revReceived();

        Delegate& _delegate;
        std::deque<Retained<MessageIn>> _waitingChangesMessages; // Queued 'changes' messages
        unsigned _pendingRevMessages {0};   // # of 'rev' msgs expected but not yet being processed
        bool _announcedDeltaSupport {false};                // Did I send "deltas:true" yet?
#ifdef LITECORE_SIGNPOSTS
        bool _changesBackPressure {false};
#endif
    };

} }
