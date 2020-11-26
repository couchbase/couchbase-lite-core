//
// Pusher+Revs.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#include "Pusher.hh"
#include "DBAccess.hh"
#include "ReplicatorTuning.hh"
#include "BLIP.hh"
#include "HTTPTypes.hh"
#include "Increment.hh"
#include "StringUtil.hh"

using namespace std;
using namespace litecore::blip;

namespace litecore::repl {

    void Pusher::maybeSendMoreRevs() {
        while (_revisionsInFlight < tuning::kMaxRevsInFlight
                   && _revisionBytesAwaitingReply <= tuning::kMaxRevBytesAwaitingReply
                   && !_revQueue.empty()) {
            Retained<RevToSend> first = move(_revQueue.front());
            _revQueue.pop_front();
            sendRevision(first);
            if (_revQueue.size() == tuning::kMaxRevsQueued - 1)
                maybeGetMoreChanges();          // I may now be eligible to send more changes
        }
//        if (!_revQueue.empty())
//            logVerbose("Throttling sending revs; _revisionsInFlight=%u/%u, _revisionBytesAwaitingReply=%llu/%u",
//                       _revisionsInFlight, tuning::kMaxRevsInFlight,
//                       _revisionBytesAwaitingReply, tuning::kMaxRevBytesAwaitingReply);
    }


    // Send a "rev" message containing a revision body.
    void Pusher::sendRevision(Retained<RevToSend> request) {
        if (!connected())
            return;

        logVerbose("Sending rev %.*s %.*s (seq #%" PRIu64 ") [%d/%d]",
                   SPLAT(request->docID), SPLAT(request->revID), request->sequence,
                   _revisionsInFlight, tuning::kMaxRevsInFlight);

        // Get the document & revision:
        C4Error c4err;
        slice revisionBody;
        Dict root;
        c4::ref<C4Document> doc = _db->getDoc(request->docID, &c4err);
        if (doc) {
            if (c4doc_selectRevision(doc, request->revID, true, &c4err)) {
                revisionBody = doc->selectedRev.body;
                if (!revisionBody)
                    c4err = {LiteCoreDomain, kC4ErrorNotFound};
            }
            if (revisionBody) {
                root = Value::fromData(revisionBody, kFLTrusted).asDict();
                if (!root)
                    c4err = {LiteCoreDomain, kC4ErrorCorruptData};
                request->flags = doc->selectedRev.flags;
            } else {
                if (c4err.code == kC4ErrorNotFound && c4err.domain == LiteCoreDomain)
                    revToSendIsObsolete(*request, &c4err);
            }
        }

        // Now send the BLIP message. Normally it's "rev", but if this is an error we make it
        // "norev" and include the error code:
        MessageBuilder msg(root ? "rev"_sl : "norev"_sl);
        msg.compressed = true;
        msg["id"_sl] = request->docID;
        msg["rev"_sl] = request->revID;
        msg["sequence"_sl] = request->sequence;
        if (root) {
            if (request->noConflicts)
                msg["noconflicts"_sl] = true;
            auto revisionFlags = doc->selectedRev.flags;
            if (revisionFlags & kRevDeleted)
                msg["deleted"_sl] = "1"_sl;
            string history = request->historyString(doc);
            if (!history.empty())
                msg["history"_sl] = history;

            bool sendLegacyAttachments = (request->legacyAttachments
                                          && (revisionFlags & kRevHasAttachments)
                                          && !_db->disableBlobSupport());

            // Delta compression:
            alloc_slice delta = createRevisionDelta(doc, request, root, revisionBody.size,
                                                    sendLegacyAttachments);
            if (delta) {
                msg["deltaSrc"_sl] = doc->selectedRev.revID;
                msg.jsonBody().writeRaw(delta);
            } else if (root.empty()) {
                msg.write("{}"_sl);
            } else {
                auto &bodyEncoder = msg.jsonBody();
                if (sendLegacyAttachments)
                    _db->encodeRevWithLegacyAttachments(bodyEncoder, root,
                                                       c4rev_getGeneration(request->revID));
                else
                    bodyEncoder.writeValue(root);
            }
            logVerbose("Transmitting 'rev' message with '%.*s' #%.*s",
                       SPLAT(request->docID), SPLAT(request->revID));
            sendRequest(msg, [this, request](MessageProgress progress) {
                onRevProgress(request, progress);
            });
            increment(_revisionsInFlight);

        } else {
            // Send an error if we couldn't get the revision:
            int blipError;
            if (c4err.domain == WebSocketDomain)
                blipError = c4err.code;
            else if (c4err.domain == LiteCoreDomain && c4err.code == kC4ErrorNotFound)
                blipError = 404;
            else {
                warn("sendRevision: Couldn't get rev '%.*s' %.*s from db: %d/%d",
                     SPLAT(request->docID), SPLAT(request->revID), c4err.domain, c4err.code);
                blipError = 500;
            }
            msg["error"_sl] = blipError;
            msg.noreply = true;
            sendRequest(msg);

            doneWithRev(request, false, false);
            enqueue(FUNCTION_TO_QUEUE(Pusher::maybeSendMoreRevs));  // async call to avoid recursion
        }
    }


    // "rev" message progress callback:
    void Pusher::onRevProgress(Retained<RevToSend> rev, const MessageProgress &progress) {
        switch (progress.state) {
            case MessageProgress::kDisconnected:
                doneWithRev(rev, false, false);
                break;
            case MessageProgress::kAwaitingReply:
                logDebug("Transmitted 'rev' %.*s #%.*s (seq #%llu)",
                         SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence);
                decrement(_revisionsInFlight);
                increment(_revisionBytesAwaitingReply, progress.bytesSent);
                maybeSendMoreRevs();
                break;
            case MessageProgress::kComplete: {
                decrement(_revisionBytesAwaitingReply, progress.bytesSent);
                bool synced = !progress.reply->isError();
                bool completed = true;
                enum {kNoRetry, kRetryLater, kRetryNow} retry = kNoRetry;
                if (synced) {
                    logVerbose("Completed rev %.*s #%.*s (seq #%" PRIu64 ")",
                               SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence);
                    finishedDocument(rev);
                } else {
                    // Handle an error received from the peer:
                    auto err = progress.reply->getError();
                    auto c4err = blipToC4Error(err);

                    if (c4error_mayBeTransient(c4err)) {
                        completed = false;
                    } else if (c4err == C4Error{WebSocketDomain, 403}) {
                        // CBL-123: Retry HTTP forbidden once
                        if (rev->retryCount++ == 0) {
                            completed = false;
                            if (!passive())
                                retry = kRetryLater;
                        }
                    } else if (c4err == C4Error{LiteCoreDomain, kC4ErrorDeltaBaseUnknown}
                            || c4err == C4Error{LiteCoreDomain, kC4ErrorCorruptDelta}
                            || c4err == C4Error{WebSocketDomain, int(net::HTTPStatus::UnprocessableEntity)}) {
                        // CBL-986: On delta error, retry without using delta
                        if (rev->deltaOK) {
                            rev->deltaOK = false;
                            completed = false;
                            retry = kRetryNow;
                        }
                    }

                    logError("Got %-serror response to rev '%.*s' #%.*s (seq #%" PRIu64 "): %.*s %d '%.*s'",
                             (completed ? "" : "transient "),
                             SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence,
                             SPLAT(err.domain), err.code, SPLAT(err.message));
                    finishedDocumentWithError(rev, c4err, !completed);
                    // If this is a permanent failure, like a validation error or conflict,
                    // then I've completed my duty to push it.
                }
                doneWithRev(rev, completed, synced);
                switch (retry) {
                    case kRetryNow:   retryRevs({rev}, true); break;
                    case kRetryLater: _revsToRetry.push_back(rev); break;
                    case kNoRetry:    break;
                }
                maybeSendMoreRevs();
                break;
            }
            default:
                break;
        }
    }


    // If sending a rev that's been obsoleted by a newer one, mark the sequence as complete and send
    // a 410 Gone error. (Common subroutine of sendRevision & shouldRetryConflictWithNewerAncestor.)
    void Pusher::revToSendIsObsolete(const RevToSend &request, C4Error *c4err) {
        logInfo("Revision '%.*s' #%.*s is obsolete; not sending it",
                SPLAT(request.docID), SPLAT(request.revID));
        if (!passive())
            _checkpointer.completedSequence(request.sequence);
        *c4err = {WebSocketDomain, 410}; // Gone
    }


    // Attempt to delta-compress the revision; returns JSON delta or a null slice.
    alloc_slice Pusher::createRevisionDelta(C4Document *doc, RevToSend *request,
                                            Dict root, size_t revisionSize,
                                            bool sendLegacyAttachments)
    {
        alloc_slice delta;
        if (!request->deltaOK || revisionSize < tuning::kMinBodySizeForDelta
                              || _options.disableDeltaSupport())
            return delta;

        // Find an ancestor revision known to the server:
        C4RevisionFlags ancestorFlags = 0;
        Dict ancestor;
        if (request->remoteAncestorRevID)
            ancestor = DBAccess::getDocRoot(doc, request->remoteAncestorRevID, &ancestorFlags);

        if(ancestorFlags & kRevDeleted)
            return delta;

        if (!ancestor && request->ancestorRevIDs) {
            for (auto revID : *request->ancestorRevIDs) {
                ancestor = DBAccess::getDocRoot(doc, revID, &ancestorFlags);
                if (ancestor)
                    break;
            }
        }
        if (ancestor.empty())
            return delta;

        Doc legacyOld, legacyNew;
        if (sendLegacyAttachments) {
            // If server needs legacy attachment layout, transform the bodies:
            Encoder enc;
            auto revPos = c4rev_getGeneration(request->revID);
            _db->encodeRevWithLegacyAttachments(enc, root, revPos);
            legacyNew = enc.finishDoc();
            root = legacyNew.root().asDict();

            if (ancestorFlags & kRevHasAttachments) {
                enc.reset();
                _db->encodeRevWithLegacyAttachments(enc, ancestor, revPos);
                legacyOld = enc.finishDoc();
                ancestor = legacyOld.root().asDict();
            }
        }

        delta = FLCreateJSONDelta(ancestor, root);
        if (!delta || delta.size > revisionSize * 1.2)
            return {};          // Delta failed, or is (probably) bigger than body; don't use

        if (willLog(LogLevel::Verbose)) {
            alloc_slice old (ancestor.toJSON());
            alloc_slice nuu (root.toJSON());
            logVerbose("Encoded revision as delta, saving %zd bytes:\n\told = %.*s\n\tnew = %.*s\n\tDelta = %.*s",
                       nuu.size - delta.size,
                       SPLAT(old), SPLAT(nuu), SPLAT(delta));
        }
        return delta;
    }


    // Finished sending a revision (successfully or not.)
    // `completed` - whether to mark the sequence as completed in the checkpointer
    // `synced` - whether the revision was successfully stored on the peer
    void Pusher::doneWithRev(RevToSend *rev, bool completed, bool synced) {
        if (!passive()) {
            addProgress({rev->bodySize, 0});
            if (completed) {
                _checkpointer.completedSequence(rev->sequence);

                auto lastSeq = _checkpointer.localMinSequence();
                if (lastSeq / 1000 > _lastSequenceLogged / 1000 || willLog(LogLevel::Verbose))
                    logInfo("Checkpoint now %s", _checkpointer.to_string().c_str());
                _lastSequenceLogged = lastSeq;
            }
            if (synced)
                _db->markRevSynced(const_cast<RevToSend*>(rev));
        }

        // Remove rev from _pushingDocs, and see if there's a newer revision to send next:
        Retained<RevToSend> newRev = move(rev->nextRev);
        _pushingDocs.erase(rev->docID);
        if (newRev) {
            if (synced && getForeignAncestors())
                newRev->remoteAncestorRevID = rev->revID;
            logVerbose("Now that '%.*s' %.*s is done, propose %.*s (remote %.*s) ...",
                       SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(newRev->revID),
                       SPLAT(newRev->remoteAncestorRevID));
            bool ok = false;
            if (synced && getForeignAncestors()
                && c4rev_getGeneration(newRev->revID) <= c4rev_getGeneration(rev->revID)) {
                // Don't send; it'll conflict with what's on the server
            } else {
                // Send newRev as though it had just arrived:
                if (_changesFeed.shouldPushRev(newRev)) {
                    gotOutOfOrderChange(newRev);
                    ok = true;
                }
            }
            if (!ok) {
                logVerbose("   ... nope, decided not to propose '%.*s' %.*s",
                           SPLAT(newRev->docID), SPLAT(newRev->revID));
                _checkpointer.completedSequence(newRev->sequence);
            }
        } else {
            logDebug("Done pushing '%.*s' %.*s", SPLAT(rev->docID), SPLAT(rev->revID));
        }
    }

}
