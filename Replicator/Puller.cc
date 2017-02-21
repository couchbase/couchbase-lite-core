//
//  Puller.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Puller.hh"

using namespace fleeceapi;

namespace litecore { namespace repl {


    Puller::Puller(Replicator *replicator)
    :_replicator(replicator)
    {
        setConnection(replicator->connection());
    }


    void Puller::start(std::string sinceSequence, bool continuous) {
        _lastSequence = _lastSequence;
        _continuous = continuous;
        LogTo(SyncLog, "Starting pull from remote seq %s", _lastSequence.c_str());
    }


} }
