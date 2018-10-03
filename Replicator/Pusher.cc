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
#include "DBWorker.hh"
#include "c4BlobStore.h"
#include "Error.hh"
#include "StringUtil.hh"
#include "SecureDigest.hh"
#include "BLIP.hh"
#include "make_unique.h"
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore { namespace repl {

    Pusher::Pusher(Replicator *replicator, DBWorker *dbActor)
    :Worker(replicator, "Push")
    ,_dbWorker(dbActor)
    ,_continuous(_options.push == kC4Continuous)
    ,_skipDeleted(_options.skipDeleted())
    {
        if (passive()) {
            // Passive replicator always sends "changes"
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
    void Pusher::_start(C4SequenceNumber sinceSequence) {
        log("Starting %spush from local seq #%llu",
            (_continuous ? "continuous " : ""), sinceSequence+1);
        _started = true;
        _pendingSequences.clear(sinceSequence);
        startSending(sinceSequence);
    }


    // Handles an incoming "subChanges" message: starts passive push (i.e. the peer is pulling).
    void Pusher::handleSubChanges(Retained<MessageIn> req) {
        if (!passive()) {
            warn("Ignoring 'subChanges' request from peer; I'm already pushing");
            req->respondWithError({"LiteCore"_sl, 501, "Not implemented."_sl});
            return;
        }
        auto since = max(req->intProperty("since"_sl), 0l);
        _continuous = req->boolProperty("continuous"_sl);
        _skipDeleted = req->boolProperty("activeOnly"_sl);
        log("Peer is pulling %schanges from seq #%llu",
            (_continuous ? "continuous " : ""), _lastSequence);

        auto filter = req->property("filter"_sl);
        if (filter) {
            log("Peer requested filter '%.*s'", SPLAT(filter));
            req->respondWithError({"LiteCore"_sl, kC4ErrorUnsupported,
                                   "Filtering not supported"_sl});
            return;
        }

        filterByDocIDs(req->JSONBody().asDict()["docIDs"].asArray());
        if (_docIDs)
            log("Peer requested filtering to %zu docIDs", _docIDs->size());

        req->respond();
        startSending(since);
    }


    // Starts active or passive push from the given sequence number.
    void Pusher::startSending(C4SequenceNumber sinceSequence) {
        _lastSequenceRead = _lastSequence = sinceSequence;
        maybeGetMoreChanges();
    }


    // Request another batch of changes from the db, if there aren't too many in progress
    void Pusher::maybeGetMoreChanges() {
        if (!_gettingChanges && !_caughtUp
                             && _changeListsInFlight < tuning::kMaxChangeListsInFlight
                             && _revsToSend.size() < tuning::kMaxRevsQueued) {
            _gettingChanges = true;
            increment(_changeListsInFlight); // will be decremented at start of _gotChanges
            logVerbose("Asking DB for %u changes since sequence #%llu ...",
                _changesBatchSize, _lastSequenceRead);
            _dbWorker->getChanges({_lastSequenceRead,
                                   _docIDs,
                                   _changesBatchSize,
                                   _continuous,
                                   _proposeChanges || !_proposeChangesKnown,  // getForeignAncestors
                                   _skipDeleted,                              // skipDeleted
                                   _proposeChanges},                          // skipForeign
                                  this);
            // response will be to call _gotChanges
        }
    }


    // Received a list of changes from the database [initiated in maybeGetMoreChanges]
    void Pusher::_gotChanges(std::shared_ptr<RevToSendList> changes,
                             C4SequenceNumber lastSequence,
                             C4Error err)
    {
        if (_gettingChanges) {
            _gettingChanges = false;
            decrement(_changeListsInFlight);
        }

        if (!connection())
            return;
        if (err.code)
            return gotError(err);
        _lastSequenceRead = lastSequence;
        _pendingSequences.seen(lastSequence);
        if (changes->empty()) {
            log("Found 0 changes up to #%llu", lastSequence);
            updateCheckpoint();
        } else {
            log("Read %zu local changes up to #%llu: sending '%-s' with sequences #%llu - #%llu",
                changes->size(), lastSequence,
                (_proposeChanges ? "proposeChanges" : "changes"),
                changes->at(0)->sequence, _lastSequenceRead);

            uint64_t bodySize = 0;
            for (auto i = changes->begin(); i != changes->end();) {
                RevToSend *rev = *i;
                auto delayedRev = _activeDocs.find(rev->docID);
                if (delayedRev == _activeDocs.end()) {
                    _activeDocs.insert({rev->docID, nullptr});
                    bodySize += rev->bodySize;
                    ++i;
                } else {
                    // This doc already has a revision being sent; wait till that one is done
                    logDebug("Holding off on change '%.*s' %.*s", SPLAT(rev->docID), SPLAT(rev->revID));
                    delayedRev->second = rev;
                    i = changes->erase(i);
                }
            }
            if (changes->empty())
                return;

            addProgress({0, bodySize});
        }

        // Send the "changes" request, and asynchronously handle the response:
        auto changeCount = changes->size();
        sendChanges(move(changes));

        if (changeCount < _changesBatchSize) {
            if (!_caughtUp) {
                log("Caught up, at lastSequence #%llu", _lastSequenceRead);
                _caughtUp = true;
                if (changeCount > 0 && passive()) {
                    // The protocol says catching up is signaled by an empty changes list, so send
                    // one if we didn't already:
                    sendChanges(make_unique<RevToSendList>());
                }
            }
        } else {
            maybeGetMoreChanges();
        }
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
            if (!passive())
                _pendingSequences.add(change->sequence);
            // Write the info array for this change:
            enc.beginArray();
            if (_proposeChanges) {
                enc << change->docID << change->revID;
                if (change->remoteAncestorRevID || change->bodySize > 0)
                    enc << change->remoteAncestorRevID;
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
                log("Got response for %zu local changes (sequences from %llu)",
                    changes->size(), changes->front()->sequence);
            }
            decrement(_changeListsInFlight);
            _proposeChangesKnown = true;
            MessageIn *reply = progress.reply;
            if (!proposedChanges && reply->isError()) {
                auto err = progress.reply->getError();
                if (err.code == 409 && (err.domain == "BLIP"_sl || err.domain == "HTTP"_sl)) {
                    // Caller is in no-conflict mode, wants 'proposeChanges' instead; retry
                    log("Server requires 'proposeChanges'; retrying...");
                    _proposeChanges = true;
                    sendChanges(move(changes));
                    return;
                }
            }

            // Request another batch of changes from the db:
            maybeGetMoreChanges();

            if (reply->isError())
                return gotError(reply);

            // The response contains an array that parallels the array I sent, with each item
            int maxHistory = (int)max(1l, reply->intProperty("maxHistory"_sl, kDefaultMaxHistory));
            bool legacyAttachments = !reply->boolProperty("blobs"_sl);
            auto requests = reply->JSONBody().asArray();

            unsigned index = 0;
            for (RevToSend *change : *changes) {
                bool queued = false;
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
                        _dbWorker->markRevSynced(change);
                    } else {
                        logError("Proposed rev '%.*s' #%.*s (ancestor %.*s) rejected with status %d",
                                 SPLAT(change->docID), SPLAT(change->revID),
                                 SPLAT(change->remoteAncestorRevID), status);
                        auto err = c4error_make(WebSocketDomain, status, "rejected by server"_sl);
                        documentGotError(change->docID, Dir::kPushing, err, false);
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
                    logVerbose("Queueing rev '%.*s' #%.*s (seq #%llu) [%zu queued]",
                               SPLAT(change->docID), SPLAT(change->revID), change->sequence,
                               _revsToSend.size());
                } else {
                    doneWithRev(change, true);  // unqueued, so we're done with it
                }
                ++index;
            }
            maybeSendMoreRevs();
        });
    }


#pragma mark - SENDING REVISIONS:


    void Pusher::maybeSendMoreRevs() {
        while (_revisionsInFlight < tuning::kMaxRevsInFlight
                   && _revisionBytesAwaitingReply <= tuning::kMaxRevBytesAwaitingReply
                   && !_revsToSend.empty()) {
            sendRevision(move(_revsToSend.front()));
            _revsToSend.pop_front();
            if (_revsToSend.size() == tuning::kMaxRevsQueued - 1)
                maybeGetMoreChanges();          // I may now be eligible to send more changes
        }
//        if (!_revsToSend.empty())
//            log("Throttling sending revs; _revisionsInFlight=%u, _revisionBytesAwaitingReply=%u",
//                _revisionsInFlight, _revisionBytesAwaitingReply);
    }

    
    // Tells the DBWorker to send a "rev" message containing a revision body.
    void Pusher::sendRevision(Retained<RevToSend> rev) {
        // Callback for after the peer receives the "rev" message:
        increment(_revisionsInFlight);
        logVerbose("Uploading rev %.*s %.*s (seq #%llu) [%d/%d]",
                   SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence,
                   _revisionsInFlight, tuning::kMaxRevsInFlight);
        _dbWorker->sendRevision(rev, asynchronize([=](MessageProgress progress) {
            // message progress callback:
            if (progress.state == MessageProgress::kDisconnected) {
                doneWithRev(rev, false);
                return;
            }
            if (progress.state == MessageProgress::kAwaitingReply) {
                logDebug("Uploaded rev %.*s #%.*s (seq #%llu)",
                         SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence);
                decrement(_revisionsInFlight);
                increment(_revisionBytesAwaitingReply, progress.bytesSent);
                maybeSendMoreRevs();
            }
            if (progress.state == MessageProgress::kComplete) {
                decrement(_revisionBytesAwaitingReply, progress.bytesSent);
                bool completed = !progress.reply->isError();
                if (completed) {
                    logVerbose("Completed rev %.*s #%.*s (seq #%llu)",
                               SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence);
                    if (!passive())
                        _dbWorker->markRevSynced(rev);
                    finishedDocument(rev->docID, Dir::kPushing);
                } else {
                    auto err = progress.reply->getError();
                    auto c4err = blipToC4Error(err);
                    bool transient = c4error_mayBeTransient(c4err);
                    logError("Got error response to rev %.*s %.*s (seq #%llu): %.*s %d '%.*s'",
                             SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence,
                             SPLAT(err.domain), err.code, SPLAT(err.message));
                    documentGotError(rev->docID, Dir::kPushing, c4err, transient);
                    // If this is a permanent failure, like a validation error or conflict,
                    // then I've completed my duty to push it.
                    completed = !transient;
                }
                doneWithRev(rev, completed);
                maybeSendMoreRevs();
            }
        }));
    }


    void Pusher::_couldntSendRevision(Retained<RevToSend> rev) {
        decrement(_revisionsInFlight);
        doneWithRev(rev, false);
        maybeSendMoreRevs();
    }


#pragma mark - SENDING ATTACHMENTS:


    C4ReadStream* Pusher::readBlobFromRequest(MessageIn *req,
                                              slice &digestStr,
                                              Replicator::BlobProgress &progress,
                                              C4Error *outError)
    {
        auto blobStore = _dbWorker->blobStore();
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
            logVerbose("Sending blob %.*s (length=%lld, compress=%d)",
                       SPLAT(digest), c4stream_getLength(blob, nullptr), reply.compressed);
            Retained<Replicator> repl = replicator();
            auto lastNotifyTime = actor::Timer::clock::now();
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
                auto now = actor::Timer::clock::now();
                if (done || now - lastNotifyTime > std::chrono::milliseconds(250)) {
                    lastNotifyTime = now;
                    repl->onBlobProgress(progress);
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
            sha1Context sha;
            sha1_begin(&sha);

            // First digest the length-prefixed nonce:
            slice nonce = request->body();
            if (nonce.size == 0 || nonce.size > 255) {
                request->respondWithError({"BLIP"_sl, 400, "Missing nonce"_sl});
                return;
            }
            uint8_t nonceLen = (nonce.size & 0xFF);
            sha1_add(&sha, &nonceLen, 1);
            sha1_add(&sha, nonce.buf, nonce.size);

            // Now digest the attachment itself:
            static constexpr size_t kBufSize = 8192;
            auto buf = make_unique<uint8_t[]>(kBufSize);
            size_t bytesRead;
            while ((bytesRead = c4stream_read(blob, buf.get(), kBufSize, &err)) > 0) {
                sha1_add(&sha, buf.get(), bytesRead);
            }
            buf.reset();
            blob = nullptr;
            
            if (err.code == 0) {
                // Respond with the base64-encoded digest:
                C4BlobKey proofDigest;
                static_assert(sizeof(proofDigest) == 20, "proofDigest is wrong size for SHA-1");
                sha1_end(&sha, &proofDigest);
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


    void Pusher::doneWithRev(const RevToSend *rev, bool completed) {
        if (!passive()) {
            addProgress({rev->bodySize, 0});
            if (completed) {
                _pendingSequences.remove(rev->sequence);
                updateCheckpoint();
            }
        }

        auto i = _activeDocs.find(rev->docID);
        if (i != _activeDocs.end()) {
            Retained<RevToSend> newRev = i->second;
            if (newRev) {
                i->second = nullptr;
                if (completed)
                    newRev->remoteAncestorRevID = rev->revID;
                logDebug("Now that '%.*s' %.*s is done, propose %.*s (parent %.*s) ...",
                    SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(newRev->revID),
                    SPLAT(newRev->remoteAncestorRevID));
                addProgress({0, newRev->bodySize});
                auto revs = make_unique<RevToSendList>();
                revs->push_back(newRev);
                sendChanges(move(revs));
            } else {
                _activeDocs.erase(i);
                logDebug("'%.*s' %.*s is done", SPLAT(rev->docID), SPLAT(rev->revID));
            }
        }
    }


    void Pusher::updateCheckpoint() {
        auto firstPending = _pendingSequences.first();
        auto lastSeq = firstPending ? firstPending - 1 : _pendingSequences.maxEver();
        if (lastSeq > _lastSequence) {
            if (lastSeq / 1000 > _lastSequence / 1000)
                log("Checkpoint now at #%llu", lastSeq);
            else
                logVerbose("Checkpoint now at #%llu", lastSeq);
            _lastSequence = lastSeq;
            if (replicator())
                replicator()->updatePushCheckpoint(_lastSequence);
        }
    }


    Worker::ActivityLevel Pusher::computeActivityLevel() const {
        ActivityLevel level;
        if (Worker::computeActivityLevel() == kC4Busy
                || (_started && !_caughtUp)
                || _changeListsInFlight > 0
                || _revisionsInFlight > 0
                || _blobsInFlight > 0
                || !_revsToSend.empty()
                || _revisionBytesAwaitingReply > 0) {
            level = kC4Busy;
        } else if (_options.push == kC4Continuous || isOpenServer()) {
            level = kC4Idle;
        } else {
            level = kC4Stopped;
        }
        if (SyncBusyLog.effectiveLevel() <= LogLevel::Info) {
            log("activityLevel=%-s: pendingResponseCount=%d, caughtUp=%d, changeLists=%u, revsInFlight=%u, blobsInFlight=%u, awaitingReply=%llu, revsToSend=%zu, pendingSequences=%zu",
                kC4ReplicatorActivityLevelNames[level],
                pendingResponseCount(),
                _caughtUp, _changeListsInFlight, _revisionsInFlight, _blobsInFlight,
                _revisionBytesAwaitingReply, _revsToSend.size(), _pendingSequences.size());
        }
        return level;
    }

} }
