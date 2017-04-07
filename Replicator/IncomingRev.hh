//
//  IncomingRev.hh
//  LiteCore
//
//  Created by Jens Alfke on 3/30/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include "ReplicatorTypes.hh"

namespace litecore { namespace repl {
    class DBWorker;
    class Puller;


    /** Manages pulling a single document. */
    class IncomingRev : public Worker {
    public:
        IncomingRev(Puller*, DBWorker*);

        void handleRev(blip::MessageIn* revMessage) {
            enqueue(&IncomingRev::_handleRev, retained(revMessage));
        }

        bool nonPassive() const                 {return _options.pull > kC4Passive;}

    protected:
        ActivityLevel computeActivityLevel() const override;

    private:
        void _handleRev(Retained<blip::MessageIn>);
        void insertRevision();
        void finish();
        void clear();
        virtual void _childChangedStatus(Worker *task, Status status) override;

        slice remoteSequence() const            {return _revMessage->property(slice("sequence"));}

        C4BlobStore *_blobStore;
        Puller* _puller;
        DBWorker* _dbWorker;
        Retained<blip::MessageIn> _revMessage;
        RevToInsert _rev;
        unsigned _pendingCallbacks {0};
        unsigned _pendingBlobs {0};
        C4Error _error {};
    };

} }

