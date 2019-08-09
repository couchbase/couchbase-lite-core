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
#include "c4.hh"

namespace litecore { namespace repl {

    /** Pulls a single blob. Invoked by IncomingRev. */
    class IncomingBlob : public Worker {
    public:
        IncomingBlob(Worker *parent NONNULL, C4BlobStore* NONNULL);

        void start(const PendingBlob &blob) {
            enqueue(&IncomingBlob::_start, blob);
        }

        virtual std::string loggingIdentifier() const override;

    private:
        void _start(PendingBlob);
        void writeToBlob(fleece::alloc_slice);
        void finishBlob();
        void notifyProgress(bool always);
        void closeWriter();
        virtual void onError(C4Error) override;
        virtual ActivityLevel computeActivityLevel() const override;

        C4BlobStore* const _blobStore;
        PendingBlob _blob;
        c4::ref<C4WriteStream> _writer;
        bool _busy {false};
        actor::Timer::time _lastNotifyTime;
    };
} }
