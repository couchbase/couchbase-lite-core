//
// IncomingRev.cc
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

#include "IncomingRev.hh"
#include "IncomingBlob.hh"
#include "Puller.hh"
#include "StringUtil.hh"
#include "c4BlobStore.h"
#include "c4Document+Fleece.h"
#include "Instrumentation.hh"
#include "BLIP.hh"
#include <atomic>
#include <deque>
#include <set>

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {


    IncomingRev::IncomingRev(Puller *puller)
    :Worker(puller, "inc")
    ,_puller(puller)
    {
        _passive = _options.pull <= kC4Passive;
        _important = false;
        static atomic<uint32_t> sRevSignpostCount {0};
        _serialNumber = ++sRevSignpostCount;
    }


    // Read the 'rev' message, on my actor thread:
    void IncomingRev::_handleRev(Retained<blip::MessageIn> msg) {
        Signpost::begin(Signpost::handlingRev, _serialNumber);

        // (Re)initialize state (I can be used multiple times by the Puller):
        _parent = _puller;  // Necessary because Worker clears _parent when first completed
        _provisionallyInserted = false;
        DebugAssert(_pendingCallbacks == 0 && !_currentBlob && _pendingBlobs.empty());

        // Set up to handle the current message:
        DebugAssert(!_revMessage);
        _revMessage = msg;
        _rev = new RevToInsert(this,
                               _revMessage->property("id"_sl),
                               _revMessage->property("rev"_sl),
                               _revMessage->property("history"_sl),
                               _revMessage->boolProperty("deleted"_sl),
                               _revMessage->boolProperty("noconflicts"_sl)
                                   || _options.noIncomingConflicts());
        _rev->deltaSrcRevID = _revMessage->property("deltaSrc"_sl);
        _remoteSequence = _revMessage->property(slice("sequence"));

        _peerError = (int)_revMessage->intProperty("error"_sl);
        if (_peerError) {
            // The sender had a last-minute failure getting the promised revision. Give up.
            warn("Peer was unable to send '%.*s'/%.*s: error %d",
                 SPLAT(_rev->docID), SPLAT(_rev->revID), _peerError);
            finish();
            return;
        }

        // Validate the revID and sequence:
        logVerbose("Received revision '%.*s' #%.*s (seq '%.*s')",
                   SPLAT(_rev->docID), SPLAT(_rev->revID), SPLAT(_remoteSequence));
        if (_rev->docID.size == 0 || _rev->revID.size == 0) {
            warn("Got invalid revision");
            _rev->error = c4error_make(WebSocketDomain, 400, "received invalid revision"_sl);
            finish();
            return;
        }
        if (!_remoteSequence && nonPassive()) {
            warn("Missing sequence in 'rev' message for active puller");
            _rev->error = c4error_make(WebSocketDomain, 400,
                                       "received revision with missing 'sequence'"_sl);
            finish();
            return;
        }

        if (!_rev->historyBuf && c4rev_getGeneration(_rev->revID) > 1)
            warn("Server sent no history with '%.*s' #%.*s", SPLAT(_rev->docID), SPLAT(_rev->revID));

        auto jsonBody = _revMessage->extractBody();

        if (_revMessage->noReply())
            _revMessage = nullptr;

        if (_rev->deltaSrcRevID == nullslice) {
            // It's not a delta. Convert body to Fleece and process:
            FLError err;
            Doc fleeceDoc = _db->tempEncodeJSON(jsonBody, &err);
            if(!fleeceDoc) {
                warn("Incoming rev failed to encode (Fleece error %d)", err);
                _rev->error = c4error_make(FleeceDomain, (int)err, "Incoming rev failed to encode"_sl);
                finish();
                return;
            }

            processBody(fleeceDoc, {FleeceDomain, err});
        } else if (_options.pullValidator || jsonBody.containsBytes("\"digest\""_sl)) {
            // It's a delta, but we need the entire document body now because either it has to be
            // passed to the validation function, or it may contain new blobs to download.
            logVerbose("Need to apply delta immediately for '%.*s' #%.*s ...",
                       SPLAT(_rev->docID), SPLAT(_rev->revID));
            C4Error err;
            Doc fleeceDoc = _db->applyDelta(_rev->docID, _rev->deltaSrcRevID, jsonBody, &err);
            if (!fleeceDoc && err.domain==LiteCoreDomain && err.code==kC4ErrorDeltaBaseUnknown) {
                // Don't have the body of the source revision. This might be because I'm in
                // no-conflict mode and the peer is trying to push me a now-obsolete revision.
                if (_options.noIncomingConflicts())
                    err = {WebSocketDomain, 409};
                else {
                    alloc_slice errMsg = c4error_getMessage(err);
                    warn("%.*s", SPLAT(errMsg));
                }
            }
            _rev->deltaSrcRevID = nullslice;
            processBody(fleeceDoc, err);
        } else {
            // It's a delta, but it can be applied later while inserting:
            _rev->deltaSrc = jsonBody;
            insertRevision();
        }
    }


    void IncomingRev::processBody(Doc fleeceDoc, C4Error error) {
        Assert(!_rev->deltaSrcRevID);   // This method does NOT work on deltas
        if (!fleeceDoc) {
            _rev->error = error;
            finish();
            return;
        }

        // Note: fleeceDoc is _not_ yet suitable for inserting into the
        // database because it doesn't use the same SharedKeys, but it lets us look at the doc
        // metadata and blobs.
        Dict root = fleeceDoc.root().asDict();

        // SG sends a fake revision with a "_removed":true property, to indicate that the doc is
        // no longer accessible (not in any channel the client has access to.)
        if (root["_removed"_sl].asBool())
            _rev->flags |= kRevPurged;

        // Strip out any "_"-prefixed properties like _id, just in case, and also any attachments
        // in _attachments that are redundant with blobs elsewhere in the doc:
        if (c4doc_hasOldMetaProperties(root) && !_db->disableBlobSupport()) {
            C4Error err;
            auto sk = fleeceDoc.sharedKeys();
            alloc_slice body = c4doc_encodeStrippingOldMetaProperties(root, sk, &err);
            if (!body) {
                warn("Failed to strip legacy attachments: error %d/%d", err.domain, err.code);
                _rev->error = c4error_make(WebSocketDomain, 500, "invalid legacy attachments"_sl);
                finish();
                return;
            }
            _rev->doc = Doc(body, kFLTrusted, sk);
            root = _rev->doc.root().asDict();
        } else {
            _rev->doc = fleeceDoc;
        }

        // Check for blobs, and queue up requests for any I don't have yet:
        _db->findBlobReferences(root, true, [=](FLDeepIterator i, Dict blob, const C4BlobKey &key) {
            _rev->flags |= kRevHasAttachments;
            _pendingBlobs.push_back({_rev->docID,
                                     alloc_slice(FLDeepIterator_GetPathString(i)),
                                     key,
                                     blob["length"_sl].asUnsigned(),
                                     c4doc_blobIsCompressible(blob)});
        });

        // Call the custom validation function if any:
        if (_options.pullValidator) {
            if (!_options.pullValidator(_rev->docID, _rev->revID, _rev->flags, root, _options.callbackContext)) {
                logInfo("Rejected by pull validator function");
                _rev->error = c4error_make(WebSocketDomain, 403, "rejected by validation function"_sl);
                _pendingBlobs.clear();
                finish();
                return;
            }
        }

        // Request the first blob, or if there are none, finish:
        if (!fetchNextBlob())
            insertRevision();
    }


    bool IncomingRev::fetchNextBlob() {
        auto blobStore = _db->blobStore();
        while (!_pendingBlobs.empty()) {
            PendingBlob firstPending = _pendingBlobs.front();
            _pendingBlobs.erase(_pendingBlobs.begin());
            if (c4blob_getSize(blobStore, firstPending.key) < 0) {
                if (!_currentBlob)
                    _currentBlob = new IncomingBlob(this, blobStore);
                _currentBlob->start(firstPending);
                return true;
            }
        }
        _currentBlob = nullptr;
        return false;
    }


    void IncomingRev::_childChangedStatus(Worker *task, Status status) {
        addProgress(status.progressDelta);
        if (status.level == kC4Idle) {
            if (status.error.code && !_rev->error.code)
                _rev->error = status.error;
            if (!fetchNextBlob()) {
                // All blobs completed, now finish:
                if (_rev->error.code == 0) {
                    logVerbose("All blobs received, now inserting revision");
                    insertRevision();
                } else {
                    finish();
                }
            }
        }
    }


    // Asks the DBAgent to insert the revision, then sends the reply and notifies the Puller.
    void IncomingRev::insertRevision() {
        Assert(_pendingBlobs.empty() && !_currentBlob);
        Assert(_rev->error.code == 0);
        Assert(_rev->deltaSrc || _rev->doc);
        increment(_pendingCallbacks);
        //Signpost::mark(Signpost::gotRev, _serialNumber);
        _puller->insertRevision(_rev);
    }


    void IncomingRev::revisionProvisionallyInserted() {
        // CAUTION: For performance reasons this method is called directly, without going through the
        // Actor event queue, so it runs on the Inserter's thread, NOT the IncomingRev's! Thus, it
        // needs to pay attention to thread-safety.
        _provisionallyInserted = true;
        _puller->revWasProvisionallyHandled();
    }


    void IncomingRev::_revisionInserted() {
        decrement(_pendingCallbacks);
        finish();
    }


    void IncomingRev::finish() {
        if (_revMessage) {
            MessageBuilder response(_revMessage);
            if (_rev->error.code != 0)
                response.makeError(c4ToBLIPError(_rev->error));
            _revMessage->respond(response);
            _revMessage = nullptr;
        }
        Signpost::end(Signpost::handlingRev, _serialNumber);

        if (_rev->error.code == 0 && _peerError)
            _rev->error = c4error_make(WebSocketDomain, 502, "Peer failed to send revision"_sl);

        // Free up memory now that I'm done:
        Assert(_pendingCallbacks == 0 && !_currentBlob && _pendingBlobs.empty());
        _currentBlob = nullptr;
        _pendingBlobs.clear();
        _rev->trim();

        _puller->revWasHandled(this);
    }


    Worker::ActivityLevel IncomingRev::computeActivityLevel() const {
        if (Worker::computeActivityLevel() == kC4Busy || _pendingCallbacks > 0 || _currentBlob) {
            return kC4Busy;
        } else {
            return kC4Stopped;
        }
    }

} }

