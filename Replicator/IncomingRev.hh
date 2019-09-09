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
#include <atomic>
#include <vector>

namespace litecore { namespace repl {
    class IncomingBlob;
    class Puller;
    class RevToInsert;


    /** Manages pulling a single document. */
    class IncomingRev : public Worker {
    public:
        IncomingRev(Puller*);

        // Called by the Puller:
        void handleRev(blip::MessageIn* revMessage NONNULL) {
            enqueue(&IncomingRev::_handleRev, retained(revMessage));
        }
        RevToInsert* rev() const                {return _rev;}
        alloc_slice remoteSequence() const      {return _remoteSequence;}
        bool wasProvisionallyInserted() const   {return _provisionallyInserted;}

        // Called by the Inserter:
        void revisionProvisionallyInserted();
        void revisionInserted()                 {enqueue(&IncomingRev::_revisionInserted);}

    protected:
        ActivityLevel computeActivityLevel() const override;

    private:
        bool nonPassive() const                 {return _options.pull > kC4Passive;}
        void _handleRev(Retained<blip::MessageIn>);
        void gotDeltaSrc(alloc_slice deltaSrcBody);
        void processBody(fleece::Doc, C4Error);
        bool fetchNextBlob();
        void insertRevision();
        void _revisionInserted();
        void finish();
        virtual void _childChangedStatus(Worker *task NONNULL, Status status) override;

        C4BlobStore *_blobStore;
        Puller* _puller;
        Retained<blip::MessageIn> _revMessage;
        Retained<RevToInsert> _rev;
        unsigned _pendingCallbacks {0};
        std::vector<PendingBlob> _pendingBlobs;
        Retained<IncomingBlob> _currentBlob;
        int _peerError {0};
        alloc_slice _remoteSequence;
        uint32_t _serialNumber {0};
        std::atomic<bool> _provisionallyInserted {false};
    };

} }

