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


    class Puller : public Actor {
    public:
        Puller(Replicator *replicator, bool continuous, std::string sinceSequence)
        :_replicator(replicator)
        ,_continuous(continuous)
        { }

        void start();

    private:
        Replicator* const _replicator;
        bool const _continuous;
    };


} }
