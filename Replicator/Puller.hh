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

namespace litecore { namespace repl {
    class IncomingRev;


    class Puller : public Worker {
    public:
        Puller(blip::Connection*, Replicator*, DBWorker*, Options options);

        void setSkipDeleted()                   {enqueue(&Puller::_setSkipDeleted);}

        // Starts an active pull
        void start(alloc_slice sinceSequence)   {enqueue(&Puller::_start, sinceSequence);}

        // Called only by IncomingRev
        void revWasHandled(IncomingRev *inc,
                           const alloc_slice &docID,
                           slice sequence,
                           bool complete);

    protected:
        virtual std::string loggingClassName() const override {return "Pull";}

        bool nonPassive() const                 {return _options.pull > kC4Passive;}
        virtual void _childChangedStatus(Worker *task, Status) override;
        virtual ActivityLevel computeActivityLevel() const override;
        void activityLevelChanged(ActivityLevel level);

    private:
        Replicator* replicator() const          {return (Replicator*)_parent.get();}
        void _start(alloc_slice sinceSequence);
        void handleChanges(Retained<MessageIn>);
        void handleRev(Retained<MessageIn>);
        void _revWasHandled(Retained<IncomingRev>, alloc_slice docID, alloc_slice sequence,
                            bool complete);
        void completedSequence(alloc_slice sequence);

        void _setSkipDeleted()                  {_skipDeleted = true;}

        static const unsigned kChangesBatchSize = 500;      // Number of changes in one response
        static const unsigned kMaxSpareIncomingRevs = 500;

        Retained<DBWorker> _dbActor;
        alloc_slice _lastSequence;
        bool _skipDeleted {false};
        bool _caughtUp {false};
        bool _fatalError {false};
        RemoteSequenceSet _missingSequences;
        std::vector<Retained<IncomingRev>> _spareIncomingRevs;
        unsigned _pendingCallbacks {0};
        unsigned _pendingRevMessages {0};
    };


} }
