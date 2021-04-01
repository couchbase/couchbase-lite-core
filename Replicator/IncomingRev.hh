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
#include "RemoteSequence.hh"
#include "Timer.hh"
#include <atomic>
#include <vector>

namespace litecore { namespace repl {
    class Puller;
    class RevToInsert;


    /** Manages pulling a single document. */
    class IncomingRev final : public Worker {
    public:
        IncomingRev(Puller* NONNULL);

        // Called by the Puller:
        void handleRev(blip::MessageIn* revMessage NONNULL);
        RevToInsert* rev() const                {return _rev;}
        RemoteSequence remoteSequence() const   {return _remoteSequence;}
        bool wasProvisionallyInserted() const   {return _provisionallyInserted;}
        void reset();

        // Called by the Inserter:
        void revisionProvisionallyInserted();
        void revisionInserted();

        int progressNotificationLevel() const override;

    protected:
        ActivityLevel computeActivityLevel() const override;

    private:
        void parseAndInsert(alloc_slice jsonBody);
        bool nonPassive() const                 {return _options.pull > kC4Passive;}
        void _handleRev(Retained<blip::MessageIn>);
        void gotDeltaSrc(alloc_slice deltaSrcBody);
        fleece::Doc parseBody(alloc_slice jsonBody);
        void processFleeceBody(fleece::Doc);
        void insertRevision();
        void _revisionInserted();
        void failWithError(C4Error);
        void failWithError(C4ErrorDomain, int code, slice message);
        void finish();

        // blob stuff:
        void fetchNextBlob();
        bool startBlob();
        void writeToBlob(fleece::alloc_slice);
        void finishBlob();
        void blobGotError(C4Error);
        void notifyBlobProgress(bool always);
        void closeBlobWriter();

        Puller*                     _puller;
        Retained<blip::MessageIn>   _revMessage;
        Retained<RevToInsert>       _rev;
        unsigned                    _pendingCallbacks {0};
        int                         _peerError {0};
        RemoteSequence              _remoteSequence;
        uint32_t                    _serialNumber {0};
        std::atomic<bool>           _provisionallyInserted {false};
        // blob stuff:
        std::vector<PendingBlob>    _pendingBlobs;
        std::vector<PendingBlob>::const_iterator _blob;
        std::unique_ptr<C4WriteStream> _writer;
        uint64_t                    _blobBytesWritten;
        actor::Timer::time          _lastNotifyTime;
    };

} }

