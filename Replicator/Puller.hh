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

namespace litecore { namespace repl {


    class Puller : public ReplActor {
    public:
        Puller(blip::Connection*, Replicator*, DBActor*, Options options);

        void start(std::string sinceSequence, const Replicator::Options&);

    private:

        void handleRev(Retained<MessageIn>);

        Replicator* const _replicator;
        DBActor* const _dbActor;
        Replicator::Options _options {};
        std::string _lastSequence;
    };


} }
