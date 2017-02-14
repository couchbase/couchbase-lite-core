//
//  Pusher.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Replicator.hh"
#include "Actor.hh"
#include "Base.hh"

namespace litecore { namespace repl {


    class Pusher : public Actor {
    public:
        Pusher(Replicator *replicator, bool continuous, sequence_t sinceSequence)
        :_replicator(replicator)
        ,_continuous(continuous)
        { }

        void start();

    private:
        Replicator* const _replicator;
        bool const _continuous;
    };
    
    
} }
