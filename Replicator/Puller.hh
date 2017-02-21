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
        Puller(Replicator *replicator);

        void start(std::string sinceSequence, bool continuous);

    private:

        Replicator* const _replicator;
        bool _continuous;
        std::string _lastSequence;
    };


} }
