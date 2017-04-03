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
        void requestBlob(const BlobRequest&);
        void insertBlob(const BlobRequest&, blip::MessageIn*);
        void blobCompleted(const BlobRequest&);
        void insertRevision();
        void finish();
        void clear();

        slice remoteSequence() const            {return _revMessage->property(slice("sequence"));}
        
        Puller* _puller;
        DBActor* _dbActor;
        Retained<blip::MessageIn> _revMessage;
        RevToInsert _rev;
        unsigned _pendingCallbacks {0};
        unsigned _pendingBlobs {0};
        C4Error _error {};
    };

} }

