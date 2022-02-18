//
// Pusher.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//
//  https://github.com/couchbase/couchbase-lite-core/wiki/Replication-Protocol

#include "Pusher.hh"
#include "DBAccess.hh"
#include "ReplicatorTuning.hh"
#include "Error.hh"
#include "Increment.hh"
#include "StringUtil.hh"
#include "BLIP.hh"
#include "HTTPTypes.hh"
#include "c4ExceptionUtils.hh"
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    Pusher::Pusher(Replicator *replicator, Checkpointer &checkpointer)
    :Worker(replicator, "Push")
    ,_continuous(_options->push == kC4Continuous)
    ,_checkpointer(checkpointer)
    ,_changesFeed(*this, _options, *_db, &checkpointer)
    {
        if (_options->push <= kC4Passive) {
            // Passive replicator always sends "changes"
            _passive = true;
            _proposeChanges = false;
            _proposeChangesKnown = true;
        } else if (_db->usingVersionVectors()) {
            // Always use "changes" with version vectors
            _proposeChanges = false;
            _proposeChangesKnown = true;
        } else {
            // Default: always send "proposeChanges"
            _proposeChanges = true;
            _proposeChangesKnown = true;
        }
        registerHandler("subChanges",      &Pusher::handleSubChanges);
        registerHandler("getAttachment",   &Pusher::handleGetAttachment);
        registerHandler("proveAttachment", &Pusher::handleProveAttachment);
    }


    // Begins active push, starting from the next sequence after sinceSequence
    void Pusher::_start() {
        auto sinceSequence = _checkpointer.localMinSequence();
        logInfo("Starting %spush from local seq #%" PRIu64,
            (_continuous ? "continuous " : ""), (uint64_t)sinceSequence+1);
        _started = true;
        startSending(sinceSequence);
    }


    // Handles an incoming "subChanges" message: starts passive push (i.e. the peer is pulling).
    void Pusher::handleSubChanges(Retained<MessageIn> req) {
        if (!passive()) {
            warn("Ignoring 'subChanges' request from peer; I'm already pushing");
            req->respondWithError({"LiteCore"_sl, 501, "Not implemented."_sl});
            return;
        }

        slice versioning = req->property("versioning");
        bool vv = _db->usingVersionVectors();
        if ((vv && versioning != "version-vectors")
                    || (!vv && versioning && versioning != "rev-trees")) {
            req->respondWithError({"LiteCore"_sl, 501, "Incompatible document versioning."_sl});
            return;
        }

        auto since = C4SequenceNumber(max(req->intProperty("since"_sl), 0l));
        _continuous = req->boolProperty("continuous"_sl);
        _changesFeed.setContinuous(_continuous);
        _changesFeed.setSkipDeletedDocs(req->boolProperty("activeOnly"_sl));
        logInfo("Peer is pulling %schanges from seq #%" PRIu64,
            (_continuous ? "continuous " : ""), (uint64_t)since);

        auto filter = req->property("filter"_sl);
        if (filter) {
            logInfo("Peer requested filter '%.*s'", SPLAT(filter));
            req->respondWithError({"LiteCore"_sl, kC4ErrorUnsupported,
                                   "Filtering not supported"_sl});
            return;
        }

        _changesFeed.filterByDocIDs(req->JSONBody().asDict()["docIDs"].asArray());

        req->respond();
        startSending(since);
    }


#pragma mark - GETTING CHANGES FROM THE DB:


    // Starts active or passive push from the given sequence number.
    void Pusher::startSending(C4SequenceNumber sinceSequence) {
        _lastSequenceRead = sinceSequence;
        _changesFeed.setLastSequence(_lastSequenceRead);
        _changesFeed.setFindForeignAncestors(getForeignAncestors());
        _maybeGetMoreChanges();
    }


    // Request another batch of changes from the db, if there aren't too many in progress
    void Pusher::_maybeGetMoreChanges() {
        if ((!_caughtUp || !_continuousCaughtUp)
                     && _changeListsInFlight < (_caughtUp ? 1 : tuning::kMaxChangeListsInFlight)
                     && _revQueue.size() < tuning::kMaxRevsQueued
                     && connected()) {
            _continuousCaughtUp = true;
            gotChanges(_changesFeed.getMoreChanges(tuning::kDefaultChangeBatchSize));
        }
    }


    void Pusher::gotChanges(ChangesFeed::Changes changes)
    {
        if (changes.err.code)
            return gotError(changes.err);

        // Add the revs in `changes` to `_pushingDocs`. If there's a collision that means we're
        // already sending an earlier revision of that document; in that case, put the newer
        // rev in the earlier one's `nextRev` field so it'll be processed later.
        for (auto iChange = changes.revs.begin(); iChange != changes.revs.end();) {
            auto rev = *iChange;
            auto [iDoc, isNew] = _pushingDocs.insert({rev->docID, rev});
            if (isNew) {
                ++iChange;
            } else {
                // This doc already has a revision being sent; wait till that one is done
                logVerbose("Holding off on change '%.*s' %.*s till earlier rev %.*s is done",
                           SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(iDoc->second->revID));
                iDoc->second->nextRev = rev;
                if (!_passive)
                    _checkpointer.addPendingSequence(rev->sequence);
                iChange = changes.revs.erase(iChange);  // remove from `changes`
            }
        }

        _lastSequenceRead = max(_lastSequenceRead, changes.lastSequence);

        if (changes.revs.empty()) {
            logInfo("Found 0 changes up to #%" PRIu64, (uint64_t)changes.lastSequence);
        } else {
            uint64_t bodySize = 0;
            for (auto &change : changes.revs)
                bodySize += change->bodySize;
            addProgress({0, bodySize});

            logInfo("Read %zu local changes up to #%" PRIu64 ": sending '%-s' with sequences #%" PRIu64 " - #%" PRIu64,
                    changes.revs.size(), (uint64_t)changes.lastSequence,
                    (_proposeChanges ? "proposeChanges" : "changes"),
                    (uint64_t)changes.revs.front()->sequence, (uint64_t)changes.revs.back()->sequence);
#if DEBUG
            if (willLog(LogLevel::Debug)) {
                for (auto &change : changes.revs)
                    logDebug("    - %.4" PRIu64 ": '%.*s' #%.*s (remote #%.*s)",
                             change->sequence, SPLAT(change->docID), SPLAT(change->revID),
                             SPLAT(change->remoteAncestorRevID));
            }
#endif
        }

        // Send the "changes" request:
        auto changeCount = changes.revs.size();
        sendChanges(move(changes.revs));

        if (!changes.askAgain) {
            // ChangesFeed says there are not currently any more changes, i.e. we've caught up.
            if (!_caughtUp) {
                logInfo("Caught up, at lastSequence #%" PRIu64, (uint64_t)changes.lastSequence);
                _caughtUp = true;
                if (_continuous)
                    _continuousCaughtUp = false;
                if (changeCount > 0 && passive()) {
                    // The protocol says catching up is signaled by an empty changes list, so send
                    // one if we didn't already:
                    sendChanges(RevToSendList{});
                }
            }
        } else if (_continuous) {
            // ChangesFeed says there may be more changes; clear `_continuousCaughtUp` so that
            // `maybeGetMoreChanges` will ask for more, assuming I'm not otherwise busy.
            _continuousCaughtUp = false;
        }

        maybeGetMoreChanges();
    }


    // Async call from the ChangesFeed when it observes new database changes in continuous mode
    void Pusher::_dbHasNewChanges() {
        if (!connected())
            return;
        _continuousCaughtUp = false;
        _maybeGetMoreChanges();
    }

    void Pusher::onError(C4Error err) {
        // If the database closes on replication stop, this error might happen
        // but it is inconsequential so suppress it.  It will still be logged, but
        // not in the worker's error property.
        if(err.domain != LiteCoreDomain || err.code != kC4ErrorNotOpen) {
            Worker::onError(err);
        }
    }

#pragma mark - SENDING A "CHANGES" MESSAGE & HANDLING RESPONSE:


    void Pusher::encodeRevID(Encoder &enc, slice revID) {
        if (_db->usingVersionVectors() && revID.findByte('*'))
            enc << _db->convertVersionToAbsolute(revID);
        else
            enc << revID;
    }


    // Sends a "changes" or "proposeChanges" message.
    void Pusher::sendChanges(RevToSendList &&in_changes) {
        bool const proposedChanges = _proposeChanges;
        auto changes = make_shared<RevToSendList>(move(in_changes));

        BEGIN_ASYNC()
        MessageBuilder req(proposedChanges ? "proposeChanges"_sl : "changes"_sl);
        if(proposedChanges) {
            req[kConflictIncludesRevProperty] = "true"_sl;
        }

        req.urgent = tuning::kChangeMessagesAreUrgent;
        req.compressed = !changes->empty();

        // Generate the JSON array of changes:
        auto &enc = req.jsonBody();
        enc.beginArray();
        for (RevToSend *change : *changes) {
            // Write the info array for this change:
            enc.beginArray();
            if (proposedChanges) {
                enc << change->docID;
                encodeRevID(enc, change->revID);
                slice remoteAncestorRevID = change->remoteAncestorRevID;
                if (remoteAncestorRevID || change->bodySize > 0)
                    encodeRevID(enc, remoteAncestorRevID);
                if (!_db->usingVersionVectors() && remoteAncestorRevID
                                                && C4Document::getRevIDGeneration(remoteAncestorRevID)
                                                   >= C4Document::getRevIDGeneration(change->revID)) {
                    warn("Proposed rev '%.*s' #%.*s has invalid ancestor %.*s",
                         SPLAT(change->docID), SPLAT(change->revID),
                         SPLAT(remoteAncestorRevID));
                }
            } else {
                enc << uint64_t(change->sequence) << change->docID;
                encodeRevID(enc, change->revID);
                if (change->deleted() || change->bodySize > 0)
                    enc << change->deleted();
            }
            if (change->bodySize > 0)
                enc << change->bodySize;
            enc.endArray();
        }
        enc.endArray();

        if (changes->empty()) {
            // Empty == just announcing 'caught up', so no need to get a reply
            req.noreply = true;
            sendRequest(req);
            return;
        }

        increment(_changeListsInFlight);

        //---- SEND REQUEST AND WAIT FOR REPLY ----
        AWAIT(Retained<MessageIn>, reply, sendAsyncRequest(req));
        if (!reply)
            return;

        // Got reply to the "changes" or "proposeChanges":
        if (!changes->empty()) {
            logInfo("Got response for %zu local changes (sequences from %" PRIu64 ")",
                changes->size(), (uint64_t)changes->front()->sequence);
        }
        decrement(_changeListsInFlight);
        _changesFeed.setFindForeignAncestors(getForeignAncestors());
        if (!proposedChanges && reply->isError()) {
            auto err = reply->getError();
            if (err.code == 409 && (err.domain == "BLIP"_sl || err.domain == "HTTP"_sl)) {
                if (!_proposeChanges && !_proposeChangesKnown) {
                    // Caller is in no-conflict mode, wants 'proposeChanges' instead; retry
                    logInfo("Server requires 'proposeChanges'; retrying...");
                    _proposeChanges = true;
                    _changesFeed.setFindForeignAncestors(getForeignAncestors());
                    sendChanges(move(*changes));
                } else {
                    logError("Server does not allow '%s'; giving up",
                             (_proposeChanges ? "proposeChanges" : "changes"));
                    for(RevToSend* change : *changes)
                        doneWithRev(change, false, false);
                    gotError(C4Error::make(LiteCoreDomain, kC4ErrorRemoteError,
                                "Incompatible with server replication protocol (changes)"_sl));
                }
                return;
            }
        }
        _proposeChangesKnown = true;

        // Request another batch of changes from the db:
        maybeGetMoreChanges();

        if (reply->isError()) {
            for(RevToSend* change : *changes)
                doneWithRev(change, false, false);
            gotError(reply);
            return;
        }

        // OK, now look at the successful response:
        int maxHistory = (int)max(1l, reply->intProperty("maxHistory"_sl,
                                                         tuning::kDefaultMaxHistory));
        bool legacyAttachments = !reply->boolProperty("blobs"_sl);
        if (!_deltasOK && reply->boolProperty("deltas"_sl)
                       && !_options->properties[kC4ReplicatorOptionDisableDeltas].asBool())
            _deltasOK = true;

        // The response body consists of an array that parallels the `changes` array I sent:
        Array::iterator iResponse(reply->JSONBody().asArray());
        for (RevToSend *change : *changes) {
            change->maxHistory = maxHistory;
            change->legacyAttachments = legacyAttachments;
            change->deltaOK = _deltasOK;
            bool queued = proposedChanges ? handleProposedChangeResponse(change, *iResponse)
                                          : handleChangeResponse(change, *iResponse);
            if (queued) {
                logVerbose("Queueing rev '%.*s' #%.*s (seq #%" PRIu64 ") [%zu queued]",
                           SPLAT(change->docID), SPLAT(change->revID), (uint64_t)change->sequence,
                           _revQueue.size());
            }
            if (iResponse)
                ++iResponse;
        }
        maybeSendMoreRevs();

        END_ASYNC()
    }


    // Handles peer's response to a single rev in a "changes" message. Returns true if queued.
    bool Pusher::handleChangeResponse(RevToSend *change, Value response)
    {
        if (Array ancestorArray = response.asArray(); ancestorArray) {
            // Array of the peer's known ancestors:
            for (Value a : ancestorArray)
                change->addRemoteAncestor(a.asString());
            _revQueue.push_back(change);
            return true;
        } else if (int64_t status = response.asInt(); status != 0) {
            // A nonzero integer is an error status, probably conflict:
            return handleProposedChangeResponse(change, response);
        } else {
            // Zero or null means the peer doesn't want the revision:
            doneWithRev(change, true, false);  // not queued, so we're done with it
            return false;
        }
    }


    // Handles peer's response to a single rev in a "proposeChanges" message. Returns true if queued.
    bool Pusher::handleProposedChangeResponse(RevToSend *change, Value response)
    {
        bool completed = true, synced = false;
        // Entry in "proposeChanges" response is a status code, with 0 for OK:
        int status = 0;
        slice serverRevID = nullslice;
        if(response.isInteger()) {
            status = (int)response.asInt();
        } else if(auto dict = response.asDict(); dict) {
            status = (int)dict["status"].asInt();
            serverRevID = dict["rev"].asString();
        }

        if (status == 0) {
            change->noConflicts = true;
            _revQueue.push_back(change);
            return true;
        } else if (status == 304) {
            // 304 means server has my rev already
            synced = true;
        } else if (status == 409) {
            // 409 means a push conflict
            if (_proposeChanges) {
                logInfo("Proposed rev '%.*s' #%.*s (ancestor %.*s) conflicts with server revision (%.*s)",
                        SPLAT(change->docID), SPLAT(change->revID),
                        SPLAT(change->remoteAncestorRevID), SPLAT(serverRevID));
            } else {
                logInfo("Rev '%.*s' #%.*s conflicts with newer server revision",
                        SPLAT(change->docID), SPLAT(change->revID));
            }
            
            if (shouldRetryConflictWithNewerAncestor(change, serverRevID)) {
                // I have a newer revision to send in its place:
                sendChanges(RevToSendList{change});
                return true;
            } else if (_options->pull <= kC4Passive) {
                C4Error error = C4Error::make(WebSocketDomain, 409,
                                             "conflicts with newer server revision"_sl);
                finishedDocumentWithError(change, error, false);
            } else {
                completed = false;
            }
        } else {
            // Other error:
            if (_proposeChanges) {
                logError("Proposed rev '%.*s' #%.*s (ancestor %.*s) rejected with status %d",
                         SPLAT(change->docID), SPLAT(change->revID),
                         SPLAT(change->remoteAncestorRevID), status);
            } else {
                logError("Rev '%.*s' #%.*s rejected with status %d",
                         SPLAT(change->docID), SPLAT(change->revID), status);
            }
            auto err = C4Error::make(WebSocketDomain, status, "rejected by server"_sl);
            finishedDocumentWithError(change, err, !completed);
        }

        // If I haven't returned true, above, then it's not queued so we're done with this rev:
        doneWithRev(change, completed, synced);
        return false;
    }


#pragma mark - CONFLICTS & OUT-OF-ORDER CHANGES:


    // Called after a proposed revision gets a 409 Conflict response from the server.
    // Check the document's current remote rev, and retry if it's different now.
    bool Pusher::shouldRetryConflictWithNewerAncestor(RevToSend *rev, slice receivedRevID) {
        if (!_proposeChanges)
            return false;
        try {
            Retained<C4Document> doc = _db->getDoc(rev->docID, kDocGetAll);
            if (doc && C4Document::equalRevIDs(doc->revID(), rev->revID)) {
                if(receivedRevID && receivedRevID != rev->remoteAncestorRevID) {
                    // Remote ancestor received in proposeChanges response, so try with 
                    // this one instead

                    // If the first portion of this test passes, then the rev exists in the tree.
                    // If the second portion passes, then receivedRevID is an ancestor of the 
                    // current rev ID and it is usable for a retry.
                    if(doc->selectRevision(receivedRevID, false) && 
                        doc->selectCommonAncestorRevision(rev->revID, receivedRevID)) {
                        logInfo("Remote reported different rev of '%.*s' (mine: %.*s theirs: %.*s); retrying push",
                            SPLAT(rev->docID), SPLAT(rev->remoteAncestorRevID), SPLAT(receivedRevID));
                        rev->remoteAncestorRevID = receivedRevID;
                        return true;
                    }
                }

                if(_options->pull <= kC4Passive) {
                    // None of this other stuff is relevant if there's 
                    // no puller getting stuff from the server
                    return false;
                }

                alloc_slice foreignAncestor = _db->getDocRemoteAncestor(doc);
                if (foreignAncestor && foreignAncestor != rev->remoteAncestorRevID) {
                    // Remote ancestor has changed, so retry if it's not a conflict:
                    doc->selectRevision(foreignAncestor, false);
                    if (!(doc->selectedRev().flags & kRevIsConflict)) {
                        logInfo("I see the remote rev of '%.*s' is now #%.*s; retrying push",
                                SPLAT(rev->docID), SPLAT(foreignAncestor));
                        rev->remoteAncestorRevID = foreignAncestor;
                        return true;
                    }
                } else {
                    // No change to remote ancestor, but try again later if it changes:
                    logInfo("Will try again if remote rev of '%.*s' is updated",
                            SPLAT(rev->docID));
                    _conflictsIMightRetry.emplace(rev->docID, rev);
                }
            } else {
                // Doc has changed, so this rev is obsolete
                revToSendIsObsolete(*rev);
            }
        } catchAndWarn();
        return false;
    }


    // Notified (by the Puller) that the remote revision of a document has changed:
    void Pusher::_docRemoteAncestorChanged(alloc_slice docID, alloc_slice foreignAncestor) {
        DebugAssert(_proposeChanges);   // Only used with proposeChanges mode
        if (status().level == kC4Stopped || !connected())
            return;
        auto i = _conflictsIMightRetry.find(docID);
        if (i != _conflictsIMightRetry.end()) {
            // OK, this is a potential conflict I noted in shouldRetryConflictWithNewerAncestor().
            // See if the doc is unchanged, by getting it by sequence:
            Retained<RevToSend> rev = i->second;
            _conflictsIMightRetry.erase(i);
            Retained<C4Document> doc = _db->useLocked()->getDocumentBySequence(rev->sequence);
            if (!doc || !C4Document::equalRevIDs(doc->revID(), rev->revID)) {
                // Local document has changed, so stop working on this revision:
                logVerbose("Notified that remote rev of '%.*s' is now #%.*s, but local doc has changed",
                           SPLAT(docID), SPLAT(foreignAncestor));
            } else if (doc->selectRevision(foreignAncestor, false)
                                    && !(doc->selectedRev().flags & kRevIsConflict)) {
                // The remote rev is an ancestor of my revision, so retry it:
                doc->selectCurrentRevision();
                logInfo("Notified that remote rev of '%.*s' is now #%.*s; retrying push of #%.*s",
                        SPLAT(docID), SPLAT(foreignAncestor), SPLAT(doc->revID()));
                rev->remoteAncestorRevID = foreignAncestor;
                gotOutOfOrderChange(rev);
            } else {
                // Nope, this really is a conflict:
                C4Error error = C4Error::make(WebSocketDomain, 409, "conflicts with server document"_sl);
                finishedDocumentWithError(rev, error, false);
            }
        }
    }


    // Called when DBWorker was holding up a revision until an ancestor revision finished.
    void Pusher::gotOutOfOrderChange(RevToSend* change) {
        if (!connected())
            return;
        logInfo("Read delayed local change '%.*s' #%.*s (remote #%.*s): sending '%-s' with sequence #%" PRIu64,
                SPLAT(change->docID), SPLAT(change->revID),
                SPLAT(change->remoteAncestorRevID),
                (_proposeChanges ? "proposeChanges" : "changes"),
                (uint64_t)change->sequence);
        _pushingDocs.insert({change->docID, change});
        if (!passive())
            _checkpointer.addPendingSequence(change->sequence);
        addProgress({0, change->bodySize});
        sendChanges(RevToSendList{change});
    }


#pragma mark - PROGRESS:


    void Pusher::_connectionClosed() {
        auto conflicts = move(_conflictsIMightRetry);
        if (!conflicts.empty()) {
            // OK, now I must report these as conflicts:
            _conflictsIMightRetry.clear();
            C4Error error = C4Error::make(WebSocketDomain, 409, "conflicts with server document"_sl);
            for (auto &entry : conflicts)
                finishedDocumentWithError(entry.second, error, false);
        }

        Worker::_connectionClosed();
    }


    bool Pusher::isBusy() const {
        return Worker::computeActivityLevel() == kC4Busy
            || (_started && (!_caughtUp || !_continuousCaughtUp))
            || _changeListsInFlight > 0
            || _revisionsInFlight > 0
            || _blobsInFlight > 0
            || !_revQueue.empty()
            || !_pushingDocs.empty()
            || _revisionBytesAwaitingReply > 0;
    }


    Worker::ActivityLevel Pusher::computeActivityLevel() const {
        ActivityLevel level;
        if (!connected()) {
            // Does this need a similar guard to what Puller has?  It doesn't
            // seem so since the Puller has stuff that happens even after the
            // connection is closed, while the Pusher does not seem to.
            level = kC4Stopped;
        } else if (isBusy()) {
            level = kC4Busy;
        } else if (_continuous || isOpenServer() || !_conflictsIMightRetry.empty()) {
            level = kC4Idle;
        } else {
            level = kC4Stopped;
        }
        if (SyncBusyLog.willLog(LogLevel::Info)) {
            size_t pendingSequences = _parent ? _checkpointer.pendingSequenceCount() : 0;
            logInfo("activityLevel=%-s: pendingResponseCount=%d, caughtUp=%d, changeLists=%u, revsInFlight=%u, blobsInFlight=%u, awaitingReply=%" PRIu64 ", revsToSend=%zu, pushingDocs=%zu, pendingSequences=%zu",
                    kC4ReplicatorActivityLevelNames[level],
                    pendingResponseCount(),
                    _caughtUp, _changeListsInFlight, _revisionsInFlight, _blobsInFlight,
                    _revisionBytesAwaitingReply, _revQueue.size(), _pushingDocs.size(),
                    pendingSequences);
        }
        return level;
    }

    void Pusher::afterEvent() {
        // If I would otherwise go idle or stop, but there are revs I want to retry, restart them:
        if (!_revsToRetry.empty() && connected() && !isBusy())
            retryRevs(move(_revsToRetry), false);
        Worker::afterEvent();
    }


    void Pusher::retryRevs(RevToSendList revsToRetry, bool immediate) {
        // immediate means I want to resend as soon as possible, bypassing another changes feed entry
        // (for example in the case of a failed delta merge)
        logInfo("%d documents failed to push and will be retried now", int(revsToRetry.size()));
        _caughtUp = false;
        if (immediate) {
            for (const auto& revToRetry : revsToRetry) {
                _pushingDocs.insert({revToRetry->docID, revToRetry});
                addProgress({0, revToRetry->bodySize});
            }
            _revQueue.insert(_revQueue.begin(), revsToRetry.begin(), revsToRetry.end());
        } else {
            ChangesFeed::Changes changes = {};
            changes.revs = move(revsToRetry);
            changes.lastSequence = _lastSequenceRead;
            gotChanges(move(changes));
        }
    }

} }
