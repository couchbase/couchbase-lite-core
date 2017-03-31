//
//  Puller.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Replicator.hh"
#include "Actor.hh"
#include "RemoteSequenceSet.hh"

namespace litecore { namespace repl {
    class IncomingRev;


    class Puller : public ReplActor {
    public:
        Puller(blip::Connection*, Replicator*, DBActor*, Options options);

        // Starts an active pull
        void start(alloc_slice sinceSequence)   {enqueue(&Puller::_start, sinceSequence);}

        // Called only by IncomingRev
        void revWasHandled(IncomingRev *inc, slice sequence, bool complete);

    protected:
        bool nonPassive() const                 {return _options.pull > kC4Passive;}
        virtual ActivityLevel computeActivityLevel() const;
        void activityLevelChanged(ActivityLevel level);

    private:
        void _start(alloc_slice sinceSequence);
        void handleChanges(Retained<MessageIn>);
        void handleRev(Retained<MessageIn>);
        void _revWasHandled(Retained<IncomingRev>, alloc_slice sequence, bool complete);

        static const unsigned kChangesBatchSize = 500;      // Number of changes in one response
        static const unsigned kMaxSpareIncomingRevs = 500;

        DBActor* const _dbActor;
        alloc_slice _lastSequence;
        bool _caughtUp {false};
        RemoteSequenceSet _requestedSequences;
        std::vector<Retained<IncomingRev>> _spareIncomingRevs;
        unsigned _pendingCallbacks {0};
    };


} }
