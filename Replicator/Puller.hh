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


    class Puller : public ReplActor {
    public:
        Puller(blip::Connection*, Replicator*, DBActor*, Options options);

        void start(alloc_slice sinceSequence);

    protected:
        bool active() const                     {return _options.pull > kC4Passive;}
        virtual bool isBusy() const override;
        virtual void afterEvent() override;

    private:
        void handleChanges(Retained<MessageIn>);
        void handleRev(Retained<MessageIn>);
        void markComplete(const alloc_slice &sequence);

        Replicator* const _replicator;
        DBActor* const _dbActor;
        alloc_slice _lastSequence;
        bool _caughtUp {false};
        RemoteSequenceSet _requestedSequences;
        unsigned _pendingCallbacks {0};
    };


} }
