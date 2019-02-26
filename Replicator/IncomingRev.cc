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
#include "DBWorker.hh"
#include "Puller.hh"
#include "StringUtil.hh"
#include "c4Document+Fleece.h"
#include "BLIP.hh"
#include <deque>
#include <set>

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {


    IncomingRev::IncomingRev(Puller *puller, DBWorker *dbWorker)
    :Worker(puller, "inc")
    ,_puller(puller)
    ,_dbWorker(dbWorker)
    {
        _important = false;
    }


    // Read the 'rev' message, on my actor thread:
    void IncomingRev::_handleRev(Retained<blip::MessageIn> msg) {
        DebugAssert(!_revMessage);
        _parent = _puller;  // Necessary because Worker clears _parent when first completed

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

        auto jsonBody = _revMessage->body();

        if (_rev->deltaSrcRevID == nullslice) {
            // It's not a delta. Convert body to Fleece and process:
            FLError err;
            auto fleeceDoc = Doc::fromJSON(jsonBody, &err);
            processBody(fleeceDoc, {FleeceDomain, err});
        } else if (_options.pullValidator || _revMessage->body().contains("\"digest\""_sl)) {
            // It's a delta, but we need the entire document body now because either it has to be
            // passed to the validation function, or it may contain new blobs to download.
            // So we call the DBWorker to (asynchronously) apply the delta, and then processBody():
            logVerbose("Need to apply delta immediately for '%.*s' #%.*s ...",
                       SPLAT(_rev->docID), SPLAT(_rev->revID));
            _dbWorker->applyDelta(_rev, _rev->deltaSrcRevID, jsonBody,
                                  asynchronize([this](Doc fleeceDoc, C4Error err) {
                _rev->deltaSrcRevID = nullslice;
                processBody(fleeceDoc, err);
            }));
        } else {
            // It's a delta, but it can be applied later while inserting:
            _rev->body = jsonBody;
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
        // database because it doesn't use the SharedKeys, but it lets us look at the doc
        // metadata and blobs.
        Dict root = fleeceDoc.root().asDict();

        // SG sends a fake revision with a "_removed":true property, to indicate that the doc is
        // no longer accessible (not in any channel the client has access to.)
        if (root["_removed"_sl].asBool())
            _rev->flags |= kRevPurged;

        // Strip out any "_"-prefixed properties like _id, just in case, and also any attachments
        // in _attachments that are redundant with blobs elsewhere in the doc:
        if (c4doc_hasOldMetaProperties(root) && !_dbWorker->disableBlobSupport()) {
            C4Error err;
            _rev->body = c4doc_encodeStrippingOldMetaProperties(root, nullptr, &err);
            if (!_rev->body) {
                warn("Failed to strip legacy attachments: error %d/%d", err.domain, err.code);
                _rev->error = c4error_make(WebSocketDomain, 500, "invalid legacy attachments"_sl);
            }
            root = Value::fromData(_rev->body, kFLTrusted).asDict();
        } else {
            _rev->body = fleeceDoc.allocedData();
        }

        // Check for blobs, and queue up requests for any I don't have yet:
        _dbWorker->findBlobReferences(root, true, [=](FLDeepIterator i, Dict blob, const C4BlobKey &key) {
            _rev->flags |= kRevHasAttachments;
            _pendingBlobs.push_back({_rev->docID,
                                     alloc_slice(FLDeepIterator_GetPathString(i)),
                                     key,
                                     blob["length"_sl].asUnsigned(),
                                     c4doc_blobIsCompressible(blob)});
        });

        // Call the custom validation function if any:
        if (_options.pullValidator) {
            if (!_options.pullValidator(_rev->docID, _rev->flags, root, _options.callbackContext)) {
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
        auto blobStore = _dbWorker->blobStore();
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
        increment(_pendingCallbacks);
        _dbWorker->insertRevision(_rev);
    }


    void IncomingRev::_revisionInserted() {
        decrement(_pendingCallbacks);
        finish();
    }


    void IncomingRev::finish() {
        if (!_revMessage->noReply()) {
            MessageBuilder response(_revMessage);
            if (_rev->error.code != 0)
                response.makeError(c4ToBLIPError(_rev->error));
            _revMessage->respond(response);
        }
        
        if (_rev->error.code == 0 && _peerError)
                _rev->error = c4error_make(WebSocketDomain, 502, "Peer failed to send revision"_sl);

        // Free up memory now that I'm done:
        Assert(_pendingCallbacks == 0 && !_currentBlob && _pendingBlobs.empty());
        _revMessage = nullptr;
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

