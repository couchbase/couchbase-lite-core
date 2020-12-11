//
// Pusher.cc
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
//  https://github.com/couchbase/couchbase-lite-core/wiki/Replication-Protocol

#include "Pusher.hh"
#include "DBAccess.hh"
#include "ReplicatorTuning.hh"
#include "Error.hh"
#include "Increment.hh"
#include "StringUtil.hh"
#include "BLIP.hh"
#include "HTTPTypes.hh"
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    Pusher::Pusher(Replicator *replicator, Checkpointer &checkpointer)
    :Worker(replicator, "Push")
    ,_continuous(_options.push == kC4Continuous)
    ,_checkpointer(checkpointer)
    ,_changesFeed(*this, _options, *_db, &checkpointer)
    {
        if (_options.push <= kC4Passive) {
            // Passive replicator always sends "changes"
            _passive = true;
            _proposeChanges = false;
            _proposeChangesKnown = true;
        } else if (_options.properties[kC4ReplicatorOptionOutgoingConflicts].asBool()) {
            // Outgoing conflicts allowed: try "changes" 1st, but server may force "proposeChanges"
            _proposeChanges = false;
            _proposeChangesKnown = false;
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
            (_continuous ? "continuous " : ""), sinceSequence+1);
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
        C4SequenceNumber since = max(req->intProperty("since"_sl), 0l);
        _continuous = req->boolProperty("continuous"_sl);
        _changesFeed.setContinuous(_continuous);
        _changesFeed.setSkipDeletedDocs(req->boolProperty("activeOnly"_sl));
        logInfo("Peer is pulling %schanges from seq #%" PRIu64,
            (_continuous ? "continuous " : ""), since);

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
            logInfo("Found 0 changes up to #%" PRIu64, changes.lastSequence);
        } else {
            uint64_t bodySize = 0;
            for (auto &change : changes.revs)
                bodySize += change->bodySize;
            addProgress({0, bodySize});

            logInfo("Read %zu local changes up to #%" PRIu64 ": sending '%-s' with sequences #%" PRIu64 " - #%" PRIu64,
                    changes.revs.size(), changes.lastSequence,
                    (_proposeChanges ? "proposeChanges" : "changes"),
                    changes.revs.front()->sequence, changes.revs.back()->sequence);
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
        sendChanges(changes.revs);

        if (changeCount < tuning::kDefaultChangeBatchSize) {
            if (!_caughtUp) {
                logInfo("Caught up, at lastSequence #%" PRIu64, changes.lastSequence);
                _caughtUp = true;
                if (_continuous)
                    _continuousCaughtUp = false;
                if (changeCount > 0 && passive()) {
                    // The protocol says catching up is signaled by an empty changes list, so send
                    // one if we didn't already:
                    RevToSendList empty;
                    sendChanges(empty);
                }
            }
        } else if (_continuous) {
            // Got a full batch of changes, so assume there are more
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


#pragma mark - SENDING A "CHANGES" MESSAGE & HANDLING RESPONSE:


    // Sends a "changes" or "proposeChanges" message.
    void Pusher::sendChanges(RevToSendList &changes) {
        MessageBuilder req(_proposeChanges ? "proposeChanges"_sl : "changes"_sl);
        req.urgent = tuning::kChangeMessagesAreUrgent;
        req.compressed = !changes.empty();

        // Generate the JSON array of changes:
        auto &enc = req.jsonBody();
        enc.beginArray();
        for (RevToSend *change : changes) {
            // Write the info array for this change:
            enc.beginArray();
            if (_proposeChanges) {
                enc << change->docID << change->revID;
                slice remoteAncestorRevID = change->remoteAncestorRevID;
                if (remoteAncestorRevID || change->bodySize > 0)
                    enc << remoteAncestorRevID;
                if (remoteAncestorRevID && c4rev_getGeneration(remoteAncestorRevID)
                                                >= c4rev_getGeneration(change->revID)) {
                    warn("Proposed rev '%.*s' #%.*s has invalid ancestor %.*s",
                         SPLAT(change->docID), SPLAT(change->revID),
                         SPLAT(remoteAncestorRevID));
                }
            } else {
                enc << change->sequence << change->docID << change->revID;
                if (change->deleted() || change->bodySize > 0)
                    enc << change->deleted();
            }
            if (change->bodySize > 0)
                enc << change->bodySize;
            enc.endArray();
        }
        enc.endArray();

        if (changes.empty()) {
            // Empty == just announcing 'caught up', so no need to get a reply
            req.noreply = true;
            sendRequest(req);
            return;
        }

        bool proposedChanges = _proposeChanges;

        increment(_changeListsInFlight);
        sendRequest(req, [this,changes=move(changes),proposedChanges](MessageProgress progress) mutable {
            if (progress.state == MessageProgress::kComplete)
                handleChangesResponse(changes, progress.reply, proposedChanges);
        });
    }


    // Handles the peer's response to a "changes" or "proposeChanges" message:
    void Pusher::handleChangesResponse(RevToSendList &changes,
                                        MessageIn *reply,
                                        bool proposedChanges)
    {
        // Got reply to the "changes" or "proposeChanges":
        if (!changes.empty()) {
            logInfo("Got response for %zu local changes (sequences from %" PRIu64 ")",
                changes.size(), changes.front()->sequence);
        }
        decrement(_changeListsInFlight);
        _proposeChangesKnown = true;
        _changesFeed.setFindForeignAncestors(getForeignAncestors());
        if (!proposedChanges && reply->isError()) {
            auto err = reply->getError();
            if (err.code == 409 && (err.domain == "BLIP"_sl || err.domain == "HTTP"_sl)) {
                // Caller is in no-conflict mode, wants 'proposeChanges' instead; retry
                logInfo("Server requires 'proposeChanges'; retrying...");
                _proposeChanges = true;
                _changesFeed.setFindForeignAncestors(getForeignAncestors());
                sendChanges(changes);
                return;
            }
        }

        // Request another batch of changes from the db:
        maybeGetMoreChanges();

        if (reply->isError()) {
            for(RevToSend* change : changes) {
                doneWithRev(change, false, false);
            }
            gotError(reply);
            return;
        }

        // OK, now look at the successful response:
        int maxHistory = (int)max(1l, reply->intProperty("maxHistory"_sl,
                                                         tuning::kDefaultMaxHistory));
        bool legacyAttachments = !reply->boolProperty("blobs"_sl);
        if (!_deltasOK && reply->boolProperty("deltas"_sl)
                       && !_options.properties[kC4ReplicatorOptionDisableDeltas].asBool())
            _deltasOK = true;

        // The response body consists of an array that parallels the `changes` array I sent:
        Array::iterator iResponse(reply->JSONBody().asArray());
        for (RevToSend *change : changes) {
            change->maxHistory = maxHistory;
            change->legacyAttachments = legacyAttachments;
            change->deltaOK = _deltasOK;
            bool queued = proposedChanges ? handleProposedChangeResponse(change, *iResponse)
                                          : handleChangeResponse(change, *iResponse);
            if (queued) {
                logVerbose("Queueing rev '%.*s' #%.*s (seq #%" PRIu64 ") [%zu queued]",
                           SPLAT(change->docID), SPLAT(change->revID), change->sequence,
                           _revQueue.size());
            }
            if (iResponse)
                ++iResponse;
        }
        maybeSendMoreRevs();
    }


    // Handles peer's response to a single rev in a "changes" message.
    bool Pusher::handleChangeResponse(RevToSend *change, Value response)
    {
        // Entry in "changes" response is an array of known ancestors, or null to skip:
        if (Array ancestorArray = response.asArray(); ancestorArray) {
            for (Value a : ancestorArray)
                change->addRemoteAncestor(a.asString());
            _revQueue.push_back(change);
            return true;
        } else {
            doneWithRev(change, true, false);  // not queued, so we're done with it
            return false;
        }
    }


    // Handles peer's response to a single rev in a "proposeChanges" message.
    bool Pusher::handleProposedChangeResponse(RevToSend *change, Value response)
    {
        bool completed = true, synced = false;
        // Entry in "proposeChanges" response is a status code, with 0 for OK:
        int status = (int)response.asInt();
        if (status == 0) {
            change->noConflicts = true;
            _revQueue.push_back(change);
            return true;
        } else if (status == 304) {
            // 304 means server has my rev already
            synced = true;
        } else if (status == 409) {
            // 409 means a push conflict
            logInfo("Proposed rev '%.*s' #%.*s (ancestor %.*s) conflicts with newer server revision",
                    SPLAT(change->docID), SPLAT(change->revID),
                    SPLAT(change->remoteAncestorRevID));
            if (_options.pull <= kC4Passive) {
                C4Error error = c4error_make(WebSocketDomain, 409,
                                             "conflicts with newer server revision"_sl);
                finishedDocumentWithError(change, error, false);
            } else if (shouldRetryConflictWithNewerAncestor(change)) {
                // I have a newer revision to send in its place:
                RevToSendList changes = {change};
                sendChanges(changes);
                return true;
            } else {
                completed = false;
            }
        } else {
            // Other error:
            logError("Proposed rev '%.*s' #%.*s (ancestor %.*s) rejected with status %d",
                     SPLAT(change->docID), SPLAT(change->revID),
                     SPLAT(change->remoteAncestorRevID), status);
            auto err = c4error_make(WebSocketDomain, status, "rejected by server"_sl);
            finishedDocumentWithError(change, err, !completed);
        }

        doneWithRev(change, completed, synced);  // not queued, so we're done with it
        return false;
    }


#pragma mark - CONFLICTS & OUT-OF-ORDER CHANGES:


    // Called after a proposed revision gets a 409 Conflict response from the server.
    // Check the document's current remote rev, and retry if it's different now.
    bool Pusher::shouldRetryConflictWithNewerAncestor(RevToSend *rev) {
        // None of this is relevant if there's no puller getting stuff from the server
        DebugAssert(_options.pull > kC4Passive);

        bool retry = false;
        _db->use([&](C4Database *db) {
            C4Error error;
            c4::ref<C4Document> doc = c4doc_get(db, rev->docID, true, &error);
            if (doc && doc->revID == rev->revID) {
                alloc_slice foreignAncestor = _db->getDocRemoteAncestor(doc);
                if (foreignAncestor && foreignAncestor != rev->remoteAncestorRevID) {
                    // Remote ancestor has changed, so retry if it's not a conflict:
                    c4doc_selectRevision(doc, foreignAncestor, false, nullptr);
                    if (!(doc->selectedRev.flags & kRevIsConflict)) {
                        logInfo("I see the remote rev of '%.*s' is now #%.*s; retrying push",
                                SPLAT(rev->docID), SPLAT(foreignAncestor));
                        rev->remoteAncestorRevID = foreignAncestor;
                        retry = true;
                    }
                } else {
                    // No change to remote ancestor, but try again later if it changes:
                    logInfo("Will try again if remote rev of '%.*s' is updated",
                            SPLAT(rev->docID));
                    _conflictsIMightRetry.emplace(rev->docID, rev);
                }
            } else {
                // Doc has changed, so this rev is obsolete
                revToSendIsObsolete(*rev, &error);
            }
        });
        return retry;
    }


    // Notified (by the Puller) that the remote revision of a document has changed:
    void Pusher::_docRemoteAncestorChanged(alloc_slice docID, alloc_slice foreignAncestor) {
        if (status().level == kC4Stopped || !connected())
            return;
        auto i = _conflictsIMightRetry.find(docID);
        if (i != _conflictsIMightRetry.end()) {
            // OK, this is a potential conflict I noted in shouldRetryConflictWithNewerAncestor().
            // See if the doc is unchanged, by getting it by sequence:
            Retained<RevToSend> rev = i->second;
            _conflictsIMightRetry.erase(i);
            c4::ref<C4Document> doc = _db->use<C4Document*>([&](C4Database *db) {
                return c4doc_getBySequence(db, rev->sequence, nullptr);
            });
            if (!doc || doc->revID != rev->revID) {
                // Local document has changed, so stop working on this revision:
                logVerbose("Notified that remote rev of '%.*s' is now #%.*s, but local doc has changed",
                           SPLAT(docID), SPLAT(foreignAncestor));
            } else if (c4doc_selectRevision(doc, foreignAncestor, false, nullptr)
                                    && !(doc->selectedRev.flags & kRevIsConflict)) {
                // The remote rev is an ancestor of my revision, so retry it:
                c4doc_selectCurrentRevision(doc);
                logInfo("Notified that remote rev of '%.*s' is now #%.*s; retrying push of #%.*s",
                        SPLAT(docID), SPLAT(foreignAncestor), SPLAT(doc->revID));
                rev->remoteAncestorRevID = foreignAncestor;
                gotOutOfOrderChange(rev);
            } else {
                // Nope, this really is a conflict:
                C4Error error = c4error_make(WebSocketDomain, 409, "conflicts with server document"_sl);
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
                change->sequence);
        _pushingDocs.insert({change->docID, change});
        if (!passive())
            _checkpointer.addPendingSequence(change->sequence);
        addProgress({0, change->bodySize});
        RevToSendList changes = {change};
        sendChanges(changes);
    }


#pragma mark - PROGRESS:


    int Pusher::progressNotificationLevel() const {
        const auto repl = const_cast<Pusher*>(this)->replicatorIfAny();
        return repl ? repl->progressNotificationLevel() : 0;
    }


    void Pusher::_connectionClosed() {
        auto conflicts = move(_conflictsIMightRetry);
        if (!conflicts.empty()) {
            // OK, now I must report these as conflicts:
            _conflictsIMightRetry.clear();
            C4Error error = c4error_make(WebSocketDomain, 409, "conflicts with server document"_sl);
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
            for (const auto& revToRetry : revsToRetry)
                _pushingDocs.insert({revToRetry->docID, revToRetry});
            _revQueue.insert(_revQueue.begin(), revsToRetry.begin(), revsToRetry.end());
        } else {
            ChangesFeed::Changes changes = {};
            changes.revs = move(revsToRetry);
            changes.lastSequence = _lastSequenceRead;
            gotChanges(move(changes));
        }
    }

} }
