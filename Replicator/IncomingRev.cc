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
#include "Puller.hh"
#include "DBAccess.hh"
#include "Increment.hh"
#include "StringUtil.hh"
#include "c4BlobStore.hh"
#include "c4Document.hh"
#include "Instrumentation.hh"
#include "BLIP.hh"
#include <atomic>
#include <deque>
#include <set>

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    // Docs with JSON bodies larger than this get parsed asynchronously (off the Puller thread)
    static constexpr size_t kMaxImmediateParseSize = 32 * 1024;

    static inline bool jsonMightContainBlobs(slice json) {
        return json.containsBytes("\"digest\""_sl);
    }

    IncomingRev::IncomingRev(Puller *puller)
    :Worker(puller, "inc")
    ,_puller(puller)
    {
        _passive = _options.pull <= kC4Passive;
        _important = false;
        static atomic<uint32_t> sRevSignpostCount {0};
        _serialNumber = ++sRevSignpostCount;
    }


    // Read the 'rev' message, then parse either synchronously or asynchronously.
    // This runs on the caller's (Puller's) thread.
    void IncomingRev::handleRev(blip::MessageIn *msg) {
        Signpost::begin(Signpost::handlingRev, _serialNumber);

        // (Re)initialize state (I can be used multiple times by the Puller):
        _parent = _puller;  // Necessary because Worker clears _parent when first completed
        _provisionallyInserted = false;
        DebugAssert(_pendingCallbacks == 0 && !_writer && _pendingBlobs.empty());
        _blob = _pendingBlobs.end();

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
        slice sequenceStr = _revMessage->property(slice("sequence"));
        _remoteSequence = RemoteSequence(sequenceStr);

        _peerError = (int)_revMessage->intProperty("error"_sl);
        if (_peerError) {
            // The sender had a last-minute failure getting the promised revision. Give up.
            warn("Peer was unable to send '%.*s'/%.*s: error %d",
                 SPLAT(_rev->docID), SPLAT(_rev->revID), _peerError);
            finish();
            return;
        }

        // Validate the docID, revID, and sequence:
        logVerbose("Received revision '%.*s' #%.*s (seq '%.*s')",
                   SPLAT(_rev->docID), SPLAT(_rev->revID), SPLAT(sequenceStr));
        if (_rev->docID.size == 0) {
            failWithError(WebSocketDomain, 400, "received invalid docID ''"_sl);
            return;
        }

        auto gen = C4Document::getRevIDGeneration(_rev->revID);  // returns 0 if revID is invalid
        bool valid = (gen > 0);
        if (valid) {
            if (_db->usingVersionVectors()) {
                // Incoming version IDs must be in absolute form (no '*')
                valid = _rev->revID.findByte('@') && !_rev->revID.findByte('*');
            } else {
                valid = _rev->revID.findByte('-');
                if (valid && gen > 1 && !_rev->historyBuf)
                    warn("Server sent no history with '%.*s' #%.*s", SPLAT(_rev->docID), SPLAT(_rev->revID));
            }
        }
        if (!valid) {
            warn("Invalid version ID in 'rev': '%.*s' #%.*s", SPLAT(_rev->docID), SPLAT(_rev->revID));
            failWithError(WebSocketDomain, 400, "received invalid version ID"_sl);
            return;
        }

        if (!_remoteSequence && nonPassive()) {
            failWithError(WebSocketDomain, 400, "received 'rev' message with missing 'sequence'"_sl);
            return;
        }

        auto jsonBody = _revMessage->extractBody();
        if (_revMessage->noReply())
            _revMessage = nullptr;

        // Decide whether to continue now (on the Puller thread) or asynchronously on my own:
        if (_options.pullValidator|| jsonBody.size > kMaxImmediateParseSize
                                  || jsonMightContainBlobs(jsonBody))
            enqueue(FUNCTION_TO_QUEUE(IncomingRev::parseAndInsert), move(jsonBody));
        else
            parseAndInsert(move(jsonBody));
    }


    void IncomingRev::parseAndInsert(alloc_slice jsonBody) {
        // First create a Fleece document:
        Doc fleeceDoc;
        C4Error err = {};
        if (_rev->deltaSrcRevID == nullslice) {
            // It's not a delta. Convert body to Fleece and process:
            FLError encodeErr;
            fleeceDoc = _db->tempEncodeJSON(jsonBody, &encodeErr);
            if (!fleeceDoc)
                err = C4Error::make(FleeceDomain, (int)encodeErr, "Incoming rev failed to encode"_sl);

        } else if (_options.pullValidator || jsonMightContainBlobs(jsonBody)) {
            // It's a delta, but we need the entire document body now because either it has to be
            // passed to the validation function, or it may contain new blobs to download.
            logVerbose("Need to apply delta immediately for '%.*s' #%.*s ...",
                       SPLAT(_rev->docID), SPLAT(_rev->revID));
            fleeceDoc = _db->applyDelta(_rev->docID, _rev->deltaSrcRevID, jsonBody);
            if (!fleeceDoc) {
                // Don't have the body of the source revision. This might be because I'm in
                // no-conflict mode and the peer is trying to push me a now-obsolete revision.
                if (_options.noIncomingConflicts())
                    err = {WebSocketDomain, 409};
                else
                    err = C4Error::printf(LiteCoreDomain, kC4ErrorDeltaBaseUnknown,
                                          "Couldn't apply delta: Don't have body of '%.*s' #%.*s",
                                          SPLAT(_rev->docID), SPLAT(_rev->deltaSrcRevID));
            }
            _rev->deltaSrcRevID = nullslice;

        } else {
            // It's a delta, but it can be applied later while inserting.
            _rev->deltaSrc = jsonBody;
            insertRevision();
            return;
        }

        if (!fleeceDoc) {
            failWithError(err);
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
        if (C4Document::hasOldMetaProperties(root) && !_db->disableBlobSupport()) {
            auto sk = fleeceDoc.sharedKeys();
            alloc_slice body = C4Document::encodeStrippingOldMetaProperties(root, sk);
            if (!body) {
                failWithError(WebSocketDomain, 500, "invalid legacy attachments"_sl);
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
                                     C4Blob::isLikelyCompressible(blob)});
            _blob = _pendingBlobs.begin();
        });

        // Call the custom validation function if any:
        if (_options.pullValidator) {
            if (!_options.pullValidator(_rev->docID, _rev->revID, _rev->flags, root,
                                        _options.callbackContext)) {
                failWithError(WebSocketDomain, 403, "rejected by validation function"_sl);
                _pendingBlobs.clear();
                _blob = _pendingBlobs.end();
                return;
            }
        }

        // Request the first blob, or if there are none, insert the revision into the DB:
        if (!_pendingBlobs.empty()) {
            fetchNextBlob();
        } else {
            insertRevision();
        }
    }


    // Asks the Inserter (via the Puller) to insert the revision into the database.
    void IncomingRev::insertRevision() {
        Assert(_blob == _pendingBlobs.end());
        Assert(_rev->error.code == 0);
        Assert(_rev->deltaSrc || _rev->doc);
        increment(_pendingCallbacks);
        //Signpost::mark(Signpost::gotRev, _serialNumber);
        _puller->insertRevision(_rev);
    }


    // Called by the Inserter after inserting the revision, but before committing the transaction.
    void IncomingRev::revisionProvisionallyInserted() {
        // CAUTION: For performance reasons this method is called directly, without going through the
        // Actor event queue, so it runs on the Inserter's thread, NOT the IncomingRev's! Thus, it
        // needs to pay attention to thread-safety.
        _provisionallyInserted = true;
        _puller->revWasProvisionallyHandled();
    }


    // Called by the Inserter after the revision is safely committed to disk.
    void IncomingRev::revisionInserted() {
        Retained<IncomingRev> retainSelf = this;
        decrement(_pendingCallbacks);        
        finish();
    }


    void IncomingRev::failWithError(C4ErrorDomain domain, int code, slice message) {
        failWithError(C4Error::make(domain, code, message));
    }

    void IncomingRev::failWithError(C4Error err) {
        warn("failed with error: %s", err.description().c_str());
        Assert(err.code != 0);
        _rev->error = err;
        finish();
    }


    // Finish up, on success or failure.
    void IncomingRev::finish() {
        if(_rev->error.domain == LiteCoreDomain &&
           (_rev->error.code == kC4ErrorDeltaBaseUnknown ||
            _rev->error.code == kC4ErrorCorruptDelta)) {
            // CBL-936: Make sure that the puller knows this revision is coming again
            // NOTE: Important that this be done before _revMessage->respond to avoid
            // racing with the newly requested rev
            _puller->revReRequested(this);
        }

        if (_revMessage) {
            MessageBuilder response(_revMessage);
            if (_rev->error.code != 0)
                response.makeError(c4ToBLIPError(_rev->error));
            _revMessage->respond(response);
            _revMessage = nullptr;
        }
        Signpost::end(Signpost::handlingRev, _serialNumber);

        if (_rev->error.code == 0 && _peerError)
            _rev->error = C4Error::make(WebSocketDomain, 502, "Peer failed to send revision"_sl);

        // Free up memory now that I'm done:
        Assert(_pendingCallbacks == 0);
        closeBlobWriter();
        _pendingBlobs.clear();
        _blob = _pendingBlobs.end();
        _rev->trim();

        _puller->revWasHandled(this);
    }


    void IncomingRev::reset() {
        _rev = nullptr;
        _parent = nullptr;
        _remoteSequence = {};
    }


    int IncomingRev::progressNotificationLevel() const {
        return _puller ? _puller->progressNotificationLevel() : 0;
    }



    Worker::ActivityLevel IncomingRev::computeActivityLevel() const {
        if (Worker::computeActivityLevel() == kC4Busy || _pendingCallbacks > 0
                                                      || (_blob != _pendingBlobs.end())) {
            return kC4Busy;
        } else {
            return kC4Stopped;
        }
    }

} }

