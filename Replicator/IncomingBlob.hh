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

        void start(C4BlobKey key, uint64_t size, bool compress) {
            enqueue(&IncomingBlob::_start, key, size, compress);
        }

    private:
        void _start(C4BlobKey, uint64_t, bool compress);
        void writeToBlob(fleece::alloc_slice);
        void finishBlob();
        void closeWriter();
        virtual void onError(C4Error) override;
        virtual ActivityLevel computeActivityLevel() const override;

        C4BlobStore* const _blobStore;
        C4BlobKey _key;
        uint64_t _size;
        C4WriteStream* _writer {nullptr};
    };
} }
