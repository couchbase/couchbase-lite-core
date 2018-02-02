//
// IncomingBlob.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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
