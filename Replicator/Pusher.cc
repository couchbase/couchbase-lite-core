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
#include "c4BlobStore.h"
#include "Error.hh"
#include "StringUtil.hh"
#include "SecureDigest.hh"
#include "BLIP.hh"
#include "HTTPTypes.hh"
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore { namespace repl {

    Pusher::Pusher(Replicator *replicator, Checkpointer &checkpointer)
    :Worker(replicator, "Push")
    ,_continuous(_options.push == kC4Continuous)
    ,_skipDeleted(_options.skipDeleted())
    ,_checkpointer(checkpointer)
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
        filterByDocIDs(_options.docIDs());
        registerHandler("subChanges",      &Pusher::handleSubChanges);
        registerHandler("getAttachment",   &Pusher::handleGetAttachment);
        registerHandler("proveAttachment", &Pusher::handleProveAttachment);
    }


    // Filters the push to the docIDs in the given Fleece array.
    // If a filter already exists, the two will be intersected.
    void Pusher::filterByDocIDs(Array docIDs) {
        if (!docIDs)
            return;
        DocIDSet combined(new unordered_set<string>);
        combined->reserve(docIDs.count());
        for (Array::iterator i(docIDs); i; ++i) {
            string docID = i.value().asstring();
            if (!docID.empty() && (!_docIDs || _docIDs->find(docID) != _docIDs->end()))
                combined->insert(move(docID));
        }
        _docIDs = move(combined);
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
        _skipDeleted = req->boolProperty("activeOnly"_sl);
        logInfo("Peer is pulling %schanges from seq #%" PRIu64,
            (_continuous ? "continuous " : ""), since);

        auto filter = req->property("filter"_sl);
        if (filter) {
            logInfo("Peer requested filter '%.*s'", SPLAT(filter));
            req->respondWithError({"LiteCore"_sl, kC4ErrorUnsupported,
                                   "Filtering not supported"_sl});
            return;
        }

        filterByDocIDs(req->JSONBody().asDict()["docIDs"].asArray());
        if (_docIDs)
            logInfo("Peer requested filtering to %zu docIDs", _docIDs->size());

        req->respond();
        startSending(since);
    }


    // Starts active or passive push from the given sequence number.
    void Pusher::startSending(C4SequenceNumber sinceSequence) {
        _lastSequenceRead = sinceSequence;
        maybeGetMoreChanges();
    }


    // Request another batch of changes from the db, if there aren't too many in progress
    void Pusher::maybeGetMoreChanges() {
        if (!_gettingChanges && (!_caughtUp || _continuous)
                         && _changeListsInFlight < (_caughtUp ? 1 : tuning::kMaxChangeListsInFlight)
                         && _revsToSend.size() < tuning::kMaxRevsQueued) {
            _gettingChanges = true;
            logVerbose("Asking DB for %u changes since sequence #%" PRIu64 " ...",
                       _changesBatchSize, _lastSequenceRead);
            // Call getMoreChanges asynchronously. Response will be to call gotChanges
            enqueue(&Pusher::getMoreChanges);
        }
    }


    // Received a list of changes from the database [initiated in maybeGetMoreChanges]
    void Pusher::gotChanges(std::shared_ptr<RevToSendList> changes,
                            C4SequenceNumber lastSequence,
                            C4Error err)
    {
        _gettingChanges = false;

        if (!connected())
            return;
        if (err.code)
            return gotError(err);
        
        if (!passive() && lastSequence > _lastSequenceRead)
            _checkpointer.addPendingSequences(*changes.get(),
                                              _lastSequenceRead + 1, lastSequence);
        _lastSequenceRead = lastSequence;

        if (changes->empty()) {
            logInfo("Found 0 changes up to #%" PRIu64, lastSequence);
        } else {
            uint64_t bodySize = 0;
            for (auto &change : *changes)
                bodySize += change->bodySize;
            addProgress({0, bodySize});

            logInfo("Read %zu local changes up to #%" PRIu64 ": sending '%-s' with sequences #%" PRIu64 " - #%" PRIu64,
                    changes->size(), lastSequence,
                    (_proposeChanges ? "proposeChanges" : "changes"),
                    changes->at(0)->sequence, _lastSequenceRead);
#if DEBUG
            if (willLog(LogLevel::Debug)) {
                for (auto &change : *changes)
                    logDebug("    - %.4llu: '%.*s' #%.*s (remote #%.*s)",
                             change->sequence, SPLAT(change->docID), SPLAT(change->revID),
                             SPLAT(change->remoteAncestorRevID));
            }
#endif
        }

        // Send the "changes" request:
        auto changeCount = changes->size();
        sendChanges(move(changes));

        if (changeCount < _changesBatchSize) {
            if (!_caughtUp) {
                logInfo("Caught up, at lastSequence #%" PRIu64, _lastSequenceRead);
                _caughtUp = true;
                if (changeCount > 0 && passive()) {
                    // The protocol says catching up is signaled by an empty changes list, so send
                    // one if we didn't already:
                    sendChanges(make_unique<RevToSendList>());
                }
            }
        }

        maybeGetMoreChanges();
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
        _pushingDocs.insert({change->docID, nullptr});
        _lastSequenceRead = max(_lastSequenceRead, change->sequence);
        if (!passive())
            _checkpointer.addPendingSequence(change->sequence);
        addProgress({0, change->bodySize});
        sendChanges(make_shared<RevToSendList>(1, change));
    }


    // Subroutine of _gotChanges that actually sends a "changes" or "proposeChanges" message:
    void Pusher::sendChanges(std::shared_ptr<RevToSendList> changes) {
        MessageBuilder req(_proposeChanges ? "proposeChanges"_sl : "changes"_sl);
        req.urgent = tuning::kChangeMessagesAreUrgent;
        req.compressed = !changes->empty();

        // Generate the JSON array of changes:
        auto &enc = req.jsonBody();
        enc.beginArray();
        for (RevToSend *change : *changes) {
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

        if (changes->empty()) {
            // Empty == just announcing 'caught up', so no need to get a reply
            req.noreply = true;
            sendRequest(req);
            return;
        }

        bool proposedChanges = _proposeChanges;

        increment(_changeListsInFlight);
        sendRequest(req, [=](MessageProgress progress) {
            // Progress callback follows:
            if (progress.state != MessageProgress::kComplete)
                return;

            // Got reply to the "changes" or "proposeChanges":
            if (!changes->empty()) {
                logInfo("Got response for %zu local changes (sequences from %" PRIu64 ")",
                    changes->size(), changes->front()->sequence);
            }
            decrement(_changeListsInFlight);
            _proposeChangesKnown = true;
            MessageIn *reply = progress.reply;
            if (!proposedChanges && reply->isError()) {
                auto err = progress.reply->getError();
                if (err.code == 409 && (err.domain == "BLIP"_sl || err.domain == "HTTP"_sl)) {
                    // Caller is in no-conflict mode, wants 'proposeChanges' instead; retry
                    logInfo("Server requires 'proposeChanges'; retrying...");
                    _proposeChanges = true;
                    sendChanges(move(changes));
                    return;
                }
            }

            // Request another batch of changes from the db:
            maybeGetMoreChanges();

            if (reply->isError()) {
                for(RevToSend* change : *changes) {
                    doneWithRev(change, false, false);
                }
                
                return gotError(reply);
            }

            int maxHistory = (int)max(1l, reply->intProperty("maxHistory"_sl, kDefaultMaxHistory));
            bool legacyAttachments = !reply->boolProperty("blobs"_sl);
            if (!_deltasOK && reply->boolProperty("deltas"_sl)
                           && !_options.properties[kC4ReplicatorOptionDisableDeltas].asBool())
                _deltasOK = true;

            // The response body consists of an array that parallels the `changes` array I sent:
            auto requests = reply->JSONBody().asArray();
            unsigned index = 0;
            for (RevToSend *change : *changes) {
                bool queued = false, completed = true, synced = false;
                change->deltaOK = _deltasOK;
                if (proposedChanges) {
                    // Entry in "proposeChanges" response is a status code, with 0 for OK:
                    int status = (int)requests[index].asInt();
                    if (status == 0) {
                        change->maxHistory = maxHistory;
                        change->legacyAttachments = legacyAttachments;
                        change->noConflicts = true;
                        _revsToSend.push_back(change);
                        queued = true;
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
                            sendChanges(make_shared<RevToSendList>(1, change));
                            queued = true;
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
                } else {
                    // Entry in "changes" response is an array of known ancestors, or null to skip:
                    Array ancestorArray = requests[index].asArray();
                    if (ancestorArray) {
                        change->maxHistory = maxHistory;
                        change->legacyAttachments = legacyAttachments;
                        for (Value a : ancestorArray)
                            change->addRemoteAncestor(a.asString());
                        _revsToSend.push_back(change);
                        queued = true;
                    }
                }

                if (queued) {
                    logVerbose("Queueing rev '%.*s' #%.*s (seq #%" PRIu64 ") [%zu queued]",
                               SPLAT(change->docID), SPLAT(change->revID), change->sequence,
                               _revsToSend.size());
                } else {
                    doneWithRev(change, completed, synced);  // not queued, so we're done with it
                }
                ++index;
            }
            maybeSendMoreRevs();
        });
    }

    // Called after a proposed revision gets a 409 Conflict response from the server.
    // Check the document's current remote rev, and retry if it's different now.
    bool Pusher::shouldRetryConflictWithNewerAncestor(RevToSend *rev) {
        // None of this is relevant if there's no puller getting stuff from the server
        DebugAssert(_options.pull > kC4Passive);

        bool retry = false;
        _db->use([&](C4Database *db) {
            DBAccess::AssertDBOpen(db);

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

    void Pusher::onError(C4Error err) {
        // If the database closes on replication stop, this error might happen
        // but it is inconsequential so suppress it.  It will still be logged, but
        // not in the worker's error property.
        if(err.domain != LiteCoreDomain || err.code != kC4ErrorNotOpen) {
            Worker::onError(err);
        }
    }


#pragma mark - SENDING REVISIONS:


    void Pusher::maybeSendMoreRevs() {
        while (_revisionsInFlight < tuning::kMaxRevsInFlight
                   && _revisionBytesAwaitingReply <= tuning::kMaxRevBytesAwaitingReply
                   && !_revsToSend.empty()) {
            Retained<RevToSend> first = move(_revsToSend.front());
            _revsToSend.pop_front();
            sendRevision(first);
            if (_revsToSend.size() == tuning::kMaxRevsQueued - 1)
                maybeGetMoreChanges();          // I may now be eligible to send more changes
        }
//        if (!_revsToSend.empty())
//            logVerbose("Throttling sending revs; _revisionsInFlight=%u/%u, _revisionBytesAwaitingReply=%llu/%u",
//                       _revisionsInFlight, tuning::kMaxRevsInFlight,
//                       _revisionBytesAwaitingReply, tuning::kMaxRevBytesAwaitingReply);
    }

    
    // Send a "rev" message containing a revision body.
    void Pusher::sendRevision(Retained<RevToSend> rev) {
        increment(_revisionsInFlight);
        logVerbose("Sending rev %.*s %.*s (seq #%" PRIu64 ") [%d/%d]",
                   SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence,
                   _revisionsInFlight, tuning::kMaxRevsInFlight);
        sendRevision(rev, [=](MessageProgress progress) {
            // message progress callback:
            if (progress.state == MessageProgress::kDisconnected) {
                doneWithRev(rev, false, false);
                return;
            }
            if (progress.state == MessageProgress::kAwaitingReply) {
                logDebug("Transmitted 'rev' %.*s #%.*s (seq #%llu)",
                         SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence);
                decrement(_revisionsInFlight);
                increment(_revisionBytesAwaitingReply, progress.bytesSent);
                maybeSendMoreRevs();
            }
            if (progress.state == MessageProgress::kComplete) {
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
            }
        });
    }


    void Pusher::couldntSendRevision(RevToSend* rev) {
        decrement(_revisionsInFlight);
        doneWithRev(rev, false, false);
        enqueue(&Pusher::maybeSendMoreRevs);  // async call to avoid recursion
    }


#pragma mark - SENDING ATTACHMENTS:


    C4ReadStream* Pusher::readBlobFromRequest(MessageIn *req,
                                              slice &digestStr,
                                              Replicator::BlobProgress &progress,
                                              C4Error *outError)
    {
        auto blobStore = _db->blobStore();
        digestStr = req->property("digest"_sl);
        progress = {Dir::kPushing};
        if (!c4blob_keyFromString(digestStr, &progress.key)) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Missing or invalid 'digest'"_sl, outError);
            return nullptr;
        }
        int64_t size = c4blob_getSize(blobStore, progress.key);
        if (size < 0) {
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, "No such blob"_sl, outError);
            return nullptr;
        }
        progress.bytesTotal = size;
        return c4blob_openReadStream(blobStore, progress.key, outError);
    }


    // Incoming request to send an attachment/blob
    void Pusher::handleGetAttachment(Retained<MessageIn> req) {
        slice digest;
        Replicator::BlobProgress progress;
        C4Error err;
        C4ReadStream* blob = readBlobFromRequest(req, digest, progress, &err);
        if (blob) {
            increment(_blobsInFlight);
            MessageBuilder reply(req);
            reply.compressed = req->boolProperty("compress"_sl);
            logVerbose("Sending blob %.*s (length=%" PRId64 ", compress=%d)",
                       SPLAT(digest), c4stream_getLength(blob, nullptr), reply.compressed);
            Retained<Replicator> repl = replicator();
            auto lastNotifyTime = actor::Timer::clock::now();
            if (progressNotificationLevel() >= 2)
                repl->onBlobProgress(progress);
            reply.dataSource = [=](void *buf, size_t capacity) mutable {
                // Callback to read bytes from the blob into the BLIP message:
                // For performance reasons this is NOT run on my actor thread, so it can't access
                // my state directly; instead it calls _attachmentSent() at the end.
                C4Error err;
                bool done = false;
                ssize_t bytesRead = c4stream_read(blob, buf, capacity, &err);
                progress.bytesCompleted += bytesRead;
                if (bytesRead < capacity) {
                    c4stream_close(blob);
                    this->enqueue(&Pusher::_attachmentSent);
                    done = true;
                }
                if (err.code) {
                    this->warn("Error reading from blob: %d/%d", err.domain, err.code);
                    progress.error = {err.domain, err.code};
                    bytesRead = -1;
                    done = true;
                }
                if (progressNotificationLevel() >= 2) {
                    auto now = actor::Timer::clock::now();
                    if (done || now - lastNotifyTime > std::chrono::milliseconds(250)) {
                        lastNotifyTime = now;
                        repl->onBlobProgress(progress);
                    }
                }
                return (int)bytesRead;
            };
            req->respond(reply);
            return;
        }
        req->respondWithError(c4ToBLIPError(err));
    }


    void Pusher::_attachmentSent() {
        decrement(_blobsInFlight);
    }


    // Incoming request to prove I have an attachment that I'm pushing, without sending it:
    void Pusher::handleProveAttachment(Retained<MessageIn> request) {
        slice digest;
        Replicator::BlobProgress progress;
        C4Error err;
        c4::ref<C4ReadStream> blob = readBlobFromRequest(request, digest, progress, &err);
        if (blob) {
            logVerbose("Sending proof of attachment %.*s", SPLAT(digest));
            SHA1Builder sha;

            // First digest the length-prefixed nonce:
            slice nonce = request->body();
            if (nonce.size == 0 || nonce.size > 255) {
                request->respondWithError({"BLIP"_sl, 400, "Missing nonce"_sl});
                return;
            }
            sha << (nonce.size & 0xFF) << nonce;

            // Now digest the attachment itself:
            static constexpr size_t kBufSize = 8192;
            auto buf = make_unique<uint8_t[]>(kBufSize);
            size_t bytesRead;
            while ((bytesRead = c4stream_read(blob, buf.get(), kBufSize, &err)) > 0)
                sha << slice(buf.get(), bytesRead);
            buf.reset();
            blob = nullptr;
            
            if (err.code == 0) {
                // Respond with the base64-encoded digest:
                C4BlobKey proofDigest;
                sha.finish(proofDigest.bytes, sizeof(proofDigest.bytes));
                alloc_slice proofStr = c4blob_keyToString(proofDigest);

                MessageBuilder reply(request);
                reply.write(proofStr);
                request->respond(reply);
                return;
            }
        }

        // If we got here, we failed:
        request->respondWithError(c4ToBLIPError(err));
    }


#pragma mark - PROGRESS:


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
        }

        if (synced && _options.push > kC4Passive)
            _db->markRevSynced(const_cast<RevToSend*>(rev));

        // Remove rev from _pushingDocs, and see if there's a newer revision to send next:
        auto i = _pushingDocs.find(rev->docID);
        if (i == _pushingDocs.end()) {
            if (connected())
                warn("_donePushingRev('%.*s'): That docID is not active!", SPLAT(rev->docID));
            return;
        }

        Retained<RevToSend> newRev = i->second;
        _pushingDocs.erase(i);
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
                bool should = _db->use<bool>([&](C4Database *db) {
                    return shouldPushRev(newRev, nullptr, db);
                });
                if (should) {
                    _maxPushedSequence = max(_maxPushedSequence, rev->sequence);
                    gotOutOfOrderChange(newRev);
                    ok = true;
                }
            }
            if (!ok) {
                logVerbose("   ... nope, decided not to propose '%.*s' %.*s",
                           SPLAT(newRev->docID), SPLAT(newRev->revID));
            }
        } else {
            logDebug("Done pushing '%.*s' %.*s", SPLAT(rev->docID), SPLAT(rev->revID));
        }
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
            || (_started && !_caughtUp)
            || _changeListsInFlight > 0
            || _revisionsInFlight > 0
            || _blobsInFlight > 0
            || !_revsToSend.empty()
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
        if (SyncBusyLog.effectiveLevel() <= LogLevel::Info) {
            size_t pendingSequences = _parent ? _checkpointer.pendingSequenceCount() : 0;
            logInfo("activityLevel=%-s: pendingResponseCount=%d, caughtUp=%d, changeLists=%u, revsInFlight=%u, blobsInFlight=%u, awaitingReply=%" PRIu64 ", revsToSend=%zu, pushingDocs=%zu, pendingSequences=%zu",
                    kC4ReplicatorActivityLevelNames[level],
                    pendingResponseCount(),
                    _caughtUp, _changeListsInFlight, _revisionsInFlight, _blobsInFlight,
                    _revisionBytesAwaitingReply, _revsToSend.size(), _pushingDocs.size(),
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
        for(const auto& revToRetry : revsToRetry)
            _pushingDocs.insert({revToRetry->docID, nullptr});
        
        if(immediate) {
            for (int i = (int)revsToRetry.size() - 1; i >= 0; i--) {
                _revsToSend.push_front(revsToRetry[i]);
            }
        } else {
            gotChanges(make_shared<RevToSendList>(revsToRetry), _maxPushedSequence, {});
        }
    }

} }
