//
//  IncomingRev.hh
//  LiteCore
//
//  Created by Jens Alfke on 3/30/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "ReplActor.hh"
#include "ReplicatorTypes.hh"

namespace litecore { namespace repl {
    using slice = fleece::slice;
    using alloc_slice = fleece::alloc_slice;
    class DBActor;
    class Puller;


    /** Manages pulling a single document. */
    class IncomingRev : public ReplActor {
    public:
        IncomingRev(Puller*, DBActor*);

        void handleRev(blip::MessageIn* revMessage) {
            enqueue(&IncomingRev::_handleRev, retained(revMessage));
        }

        bool nonPassive() const                 {return _options.pull > kC4Passive;}

    protected:
        ActivityLevel computeActivityLevel() const override;

    private:
        void _handleRev(Retained<blip::MessageIn>);
        void insertRevision();
        void clear();
        
        Puller* _puller;
        DBActor* _dbActor;
        Retained<blip::MessageIn> _revMessage;
        RevToInsert _rev;
        unsigned _pendingCallbacks {0};
    };

} }

