//
// IncomingRev.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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
        void handleRev(blip::MessageIn* revMessage NONNULL, uint64_t bodySizeOfRemoteSequence);
        void handleRevokedDoc(RevToInsert*);
        RevToInsert* rev() const                {return _rev;}
        RemoteSequence remoteSequence() const   {return _remoteSequence;}
        bool wasProvisionallyInserted() const   {return _provisionallyInserted;}
        void reset();

        // Called by the Inserter:
        void revisionProvisionallyInserted();
        void revisionInserted();

    protected:
        ActivityLevel computeActivityLevel() const override;

    private:
        void reinitialize();
        void parseAndInsert(alloc_slice jsonBody);
        bool nonPassive() const                 {return _options->pull > kC4Passive;}
        void _handleRev(Retained<blip::MessageIn>);
        void gotDeltaSrc(alloc_slice deltaSrcBody);
        fleece::Doc parseBody(alloc_slice jsonBody);
        void processFleeceBody(fleece::Doc);
        bool performPullValidation(fleece::Dict body);
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
        bool                        _mayContainBlobs;
        bool                        _mayContainEncryptedProperties;
        uint64_t                    _bodySize;
    };

} }

