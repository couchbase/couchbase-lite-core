//
// IncomingRev.hh
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
#include "function_ref.hh"
#include <vector>

namespace litecore { namespace repl {
    class DBWorker;
    class IncomingBlob;
    class Puller;


    /** Manages pulling a single document. */
    class IncomingRev : public Worker {
    public:
        IncomingRev(Puller*, DBWorker*);

        void handleRev(blip::MessageIn* revMessage) {
            enqueue(&IncomingRev::_handleRev, retained(revMessage));
        }

        bool nonPassive() const                 {return _options.pull > kC4Passive;}

        static bool shouldCompress(fleece::Dict meta);

    protected:
        ActivityLevel computeActivityLevel() const override;

    private:
        void _handleRev(Retained<blip::MessageIn>);
        bool fetchNextBlob();
        void insertRevision();
        void finish();
        void clear();
        virtual void _childChangedStatus(Worker *task, Status status) override;

        slice remoteSequence() const            {return _revMessage->property(slice("sequence"));}

        C4BlobStore *_blobStore;
        Puller* _puller;
        DBWorker* _dbWorker;
        Retained<blip::MessageIn> _revMessage;
        Retained<RevToInsert> _rev;
        unsigned _pendingCallbacks {0};
        std::vector<PendingBlob> _pendingBlobs;
        Retained<IncomingBlob> _currentBlob;
        C4Error _error {};
        int _peerError {0};
    };

} }

