//
//  c4Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/17/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Fleece.h"
#include "c4Database.h"
#include "c4Replicator.h"
#include "LibWSProvider.hh"
#include "Replicator.hh"

using namespace fleece;
using namespace fleeceapi;
using namespace litecore;
using namespace litecore::repl;


namespace litecore { namespace repl {
    websocket::Provider *sWSProvider;
} }


C4Replicator* c4repl_new(C4Database* db,
                         C4Address c4addr,
                         C4ReplicateOptions c4opts,
                         C4Error *err) C4API
{
    if (!sWSProvider) {
        sWSProvider = new websocket::LibWSProvider();
    }
    websocket::Address address(asstring(c4addr.scheme),
                               asstring(c4addr.hostname),
                               c4addr.port,
                               asstring(c4addr.path));
    Replicator::Options options{ c4opts.push, c4opts.pull, c4opts.continuous };
    auto repl = new Replicator(db, *sWSProvider, address, options);
    return (C4Replicator*) retain(repl);
}
