//
//  IncomingBlob.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/4/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include "ReplicatorTypes.hh"

namespace litecore { namespace repl {

    class IncomingBlob : public Worker {
    public:
        IncomingBlob(Worker *parent, C4BlobStore*);

        void start(BlobRequest blob) {
            enqueue(&IncomingBlob::_start, blob);
        }

    private:
        void _start(BlobRequest);
        void writeToBlob(fleece::alloc_slice);
        void finishBlob();
        virtual void onError(C4Error) override;
        virtual ActivityLevel computeActivityLevel() const override;

        C4BlobStore* const _blobStore;
        BlobRequest _blob;
        C4WriteStream* _writer {nullptr};
    };
} }
