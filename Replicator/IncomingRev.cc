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
        _error = {};
        _parent = _puller;  // Necessary because Worker clears _parent when first completed

        _revMessage = msg;
        _rev = new RevToInsert(_revMessage->property("id"_sl),
                               _revMessage->property("rev"_sl),
                               _revMessage->property("history"_sl),
                               _revMessage->boolProperty("deleted"_sl),
                               _revMessage->boolProperty("noconflicts"_sl)
                                   || _options.noIncomingConflicts());
        _docID = _rev->docID;
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
            _error = c4error_make(WebSocketDomain, 400, "received invalid revision"_sl);
            finish();
            return;
        }
        if (!_remoteSequence && nonPassive()) {
            warn("Missing sequence in 'rev' message for active puller");
            _error = c4error_make(WebSocketDomain, 400,
                                  "received revision with missing 'sequence'"_sl);
            finish();
            return;
        }

        slice deltaSrcRevID = _revMessage->property("deltaSrc"_sl);
        if (deltaSrcRevID) {
            _dbWorker->applyDelta(_rev->docID, deltaSrcRevID, _revMessage->body(),
                                  asynchronize([this](alloc_slice body, C4Error err) {
                processBody(body, err);
            }));
        } else {
            FLError err;
            alloc_slice body = Doc::fromJSON(_revMessage->body(), &err).allocedData();
            processBody(body, {FleeceDomain, err});
        }
    }


    void IncomingRev::processBody(alloc_slice fleeceBody, C4Error error) {
        if (!fleeceBody) {
            _error = error;
            finish();
            return;
        }

        // Note: fleeceBody is _not_ suitable for inserting into the
        // database because it doesn't use the SharedKeys, but it lets us look at the doc
        // metadata and blobs.
        Dict root = Value::fromData(fleeceBody, kFLTrusted).asDict();

        // Strip out any "_"-prefixed properties like _id, just in case, and also any attachments
        // in _attachments that are redundant with blobs elsewhere in the doc:
        if (c4doc_hasOldMetaProperties(root) && !_dbWorker->disableBlobSupport()) {
            C4Error err;
            fleeceBody = c4doc_encodeStrippingOldMetaProperties(root, nullptr, &err);
            if (!fleeceBody) {
                warn("Failed to strip legacy attachments: error %d/%d", err.domain, err.code);
                _error = c4error_make(WebSocketDomain, 500, "invalid legacy attachments"_sl);
            }
            root = Value::fromData(fleeceBody, kFLTrusted).asDict();
        }

        // Populate the RevToInsert's body:
        _rev->body = fleeceBody;

        // Check for blobs, and queue up requests for any I don't have yet:
        _dbWorker->findBlobReferences(root, [=](FLDeepIterator i, Dict blob, const C4BlobKey &key) {
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
                _error = c4error_make(WebSocketDomain, 403, "rejected by validation function"_sl);
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
            if (status.error.code && !_error.code)
                _error = status.error;
            if (!fetchNextBlob()) {
                // All blobs completed, now finish:
                if (_error.code == 0) {
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
        _rev->onInserted = asynchronize([this](C4Error err) {
            // Callback that will run _after_ insertRevision() completes:
            decrement(_pendingCallbacks);
            _error = err;
            finish();
        });

        _dbWorker->insertRevision(_rev);
    }


    void IncomingRev::finish() {
        if (!_revMessage->noReply()) {
            MessageBuilder response(_revMessage);
            if (_error.code != 0)
                response.makeError(c4ToBLIPError(_error));
            _revMessage->respond(response);
        }
        if (_error.code == 0 && _peerError)
            _error = c4error_make(WebSocketDomain, 502, "Peer failed to send revision"_sl);
        if (_error.code) {
            documentGotError(_rev->docID, Dir::kPulling, _error, false);
        } else if (_rev->flags & kRevIsConflict) {
            // DBWorker::_insertRevision set this flag to indicate that the rev caused a conflict
            // (though it did get inserted), so notify the delegate of the conflict:
            documentGotError(_rev->docID, Dir::kPulling, {LiteCoreDomain, kC4ErrorConflict}, true);
        }

        // Free up memory now that I'm done:
        Assert(_pendingCallbacks == 0 && !_currentBlob && _pendingBlobs.empty());
        _revMessage = nullptr;
        _rev = nullptr;
        _currentBlob = nullptr;
        _pendingBlobs.clear();

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

