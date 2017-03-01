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
#include <unordered_set>

namespace litecore { namespace repl {


    class Puller : public ReplActor {
    public:
        Puller(blip::Connection*, Replicator*, DBActor*, Options options);

        void start(std::string sinceSequence);

    protected:
        virtual bool isBusy() const override;
        virtual void afterEvent() override;

    private:
        void handleChanges(Retained<MessageIn>);
        void handleRev(Retained<MessageIn>);

        Replicator* const _replicator;
        DBActor* const _dbActor;
        std::string _lastSequence;
        bool _caughtUp {false};
        std::unordered_set<fleece::alloc_slice, fleece::sliceHash> _requestedSequences;
        unsigned _pendingCallbacks {0};
    };


} }
