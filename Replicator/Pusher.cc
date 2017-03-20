//
//  Pusher.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Pusher.hh"
#include "DBActor.hh"
#include "StringUtil.hh"
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {


    Pusher::Pusher(Connection *connection, Replicator *replicator, DBActor *dbActor, Options options)
    :ReplActor(connection, options, "Push")
    ,_replicator(replicator)
    ,_dbActor(dbActor)
    ,_continuous(options.push == kC4Continuous)
    {
        registerHandler("subChanges",       &Pusher::handleSubChanges);
    }


    // Begins active push, starting from the next sequence after sinceSequence
    void Pusher::_start(C4SequenceNumber sinceSequence) {
        log("Starting %spush from local seq %llu",
            (_continuous ? "continuous " : ""), _lastSequence+1);
        _started = true;
        _pendingSequences.clear(sinceSequence);
        startSending(sinceSequence);
    }


    // Handles an incoming "subChanges" message: starts passive push (i.e. the peer is pulling).
    void Pusher::handleSubChanges(Retained<MessageIn> req) {
        if (nonPassive()) {
            warn("Ignoring 'subChanges' request from peer; I'm already pushing");
            req->respondWithError("LiteCore"_sl, kC4ErrorConflict,
                                  "I'm already pushing"_sl);     //TODO: Proper error code
            return;
        }
        auto since = max(req->intProperty("since"_sl), 0l);
        _continuous = req->boolProperty("continuous"_sl);
        log("Peer is pulling %schanges from seq %llu",
            (_continuous ? "continuous " : ""), _lastSequence);
        startSending(since);
    }


    // Starts active or passive push from the given sequence number.
    void Pusher::startSending(C4SequenceNumber sinceSequence) {
        _lastSequenceRead = _lastSequence = sinceSequence;
        maybeGetMoreChanges();
    }


    // Request another batch of changes from the db, if there aren't too many in progress
    void Pusher::maybeGetMoreChanges() {
        if (!_gettingChanges && _changeListsInFlight < kMaxChangeListsInFlight && !_caughtUp ) {
            _gettingChanges = true;
            ++_changeListsInFlight;
            log("Reading %u changes since sequence %llu ...", _changesBatchSize, _lastSequenceRead);
            _dbActor->getChanges(_lastSequenceRead, _changesBatchSize, _continuous, this);
            // response will be to call _gotChanges
        }
    }


    // Received a list of changes from the database [initiated in getMoreChanges]
    void Pusher::_gotChanges(RevList changes, C4Error err) {
        _gettingChanges = false;
        if (err.code)
            return gotError(err);
        if (!changes.empty()) {
            _lastSequenceRead = changes.back().sequence;
            log("Read %zu changes: Pusher sending 'changes' with sequences %llu - %llu",
                  changes.size(), changes[0].sequence, _lastSequenceRead);
        }

        // Send the "changes" request, and asynchronously handle the response:
        bool caughtUpWhenSent = _caughtUp;

        if (changes.empty()) {
            sendChanges(changes, nullptr);
            --_changeListsInFlight;
        } else {
            sendChanges(changes, [=](MessageProgress progress) {
                MessageIn *reply = progress.reply;
                if (!reply)
                    return;

                // Got response to the 'changes' message:
                if (!caughtUpWhenSent) {
                    assert(_changeListsInFlight >= 0);
                    --_changeListsInFlight;
                    maybeGetMoreChanges();
                }

                if (progress.reply->isError())
                    return gotError(reply);

                // The response contains an array that, for each change in the outgoing message,
                // contains either a list of known ancestors, or null/false/0 if not interested.
                int maxHistory = (int)max(0l, reply->intProperty("maxHistory"_sl));
                auto requests = reply->JSONBody().asArray();

                unsigned index = 0;
                for (auto &change : changes) {
                    Array ancestorArray = requests[index].asArray();
                    if (ancestorArray) {
                        auto request = _revsToSend.emplace(_revsToSend.end(), change, maxHistory);
                        request->ancestorRevIDs.reserve(ancestorArray.count());
                        for (Value a : ancestorArray) {
                            slice revid = a.asString();
                            if (revid)
                                request->ancestorRevIDs.emplace_back(revid);
                        }
                        logVerbose("Queueing rev %.*s #%.*s (seq %llu)",
                                   SPLAT(request->docID), SPLAT(request->revID), request->sequence);
                    } else {
                        markComplete(change.sequence);  // unwanted, so we're done with it
                    }
                    ++index;
                }
                maybeSendMoreRevs();
            });
        }

        if (changes.size() < _changesBatchSize) {
            if (!_caughtUp) {
                log("Caught up, at lastSequence %llu", _lastSequenceRead);
                _caughtUp = true;
                if (changes.size() > 0 && !nonPassive()) {
                    // The protocol says catching up is signaled by an empty changes list:
                    sendChanges(RevList(), nullptr);
                }
            }
        } else {
            maybeGetMoreChanges();
        }
    }


    // Subroutine of _gotChanges that actually sends a "changes" message:
    void Pusher::sendChanges(const RevList &changes, MessageProgressCallback onProgress) {
        MessageBuilder req("changes"_sl);
        req.urgent = kChangeMessagesAreUrgent;
        req.noreply = !onProgress;
        req.compressed = !changes.empty();
        auto &enc = req.jsonBody();
        enc.beginArray();
        for (auto &change : changes) {
            if (nonPassive())
                _pendingSequences.add(change.sequence);
            enc.beginArray();
            enc << change.sequence << change.docID << change.revID;
            enc.endArray();
        }
        enc.endArray();
        sendRequest(req, onProgress);
    }


#pragma mark - SENDING REVISIONS:


    void Pusher::maybeSendMoreRevs() {
        while (_revisionsInFlight < kMaxRevsInFlight
                   && _revisionBytesAwaitingReply <= kMaxRevBytesAwaitingReply
                   && !_revsToSend.empty()) {
            sendRevision(_revsToSend.front());
            _revsToSend.pop_front();
        }
//        if (!_revsToSend.empty())
//            log("Throttling sending revs; _revisionsInFlight=%u, _revisionBytesAwaitingReply=%u",
//                _revisionsInFlight, _revisionBytesAwaitingReply);//TEMP
    }

    
    // Subroutine of _gotChanges that sends a "rev" message containing a revision body.
    void Pusher::sendRevision(const RevRequest &rev)
    {
        MessageProgressCallback onProgress;
        if (nonPassive()) {
            // Callback for after the peer receives the "rev" message:
            ++_revisionsInFlight;
            logDebug("Uploading rev %.*s #%.*s (seq %llu)",
                SPLAT(rev.docID), SPLAT(rev.revID), rev.sequence);
            onProgress = asynchronize([=](MessageProgress progress) {
                if (progress.state == MessageProgress::kAwaitingReply) {
                    logDebug("Uploaded rev %.*s #%.*s (seq %llu)",
                             SPLAT(rev.docID), SPLAT(rev.revID), rev.sequence);
                    assert(_revisionsInFlight > 0);//TEMP
                    --_revisionsInFlight;
                    _revisionBytesAwaitingReply += progress.bytesSent;
                    maybeSendMoreRevs();
                }
                if (progress.reply) {
                    _revisionBytesAwaitingReply -= progress.bytesSent;
                    if (progress.reply->isError())
                        gotError(progress.reply);
                    else {
                        logVerbose("Completed rev %.*s #%.*s (seq %llu)",
                                   SPLAT(rev.docID), SPLAT(rev.revID), rev.sequence);
                        markComplete(rev.sequence);
                    }
                    maybeSendMoreRevs();
                }
            });
        }
        // Tell the DBAgent to actually read from the DB and send the message:
        _dbActor->sendRevision(rev, onProgress);
    }


#pragma mark - PROGRESS:


    // Records that a sequence has been successfully pushed.
    void Pusher::markComplete(C4SequenceNumber sequence) {
        if (nonPassive()) {
            _pendingSequences.remove(sequence);
            auto firstPending = _pendingSequences.first();
            auto lastSeq = firstPending ? firstPending - 1 : _pendingSequences.maxEver();
            if (lastSeq > _lastSequence) {
                if (lastSeq / 100 > _lastSequence / 100)
                    log("Checkpoint now at %llu", lastSeq);
                else
                    logVerbose("Checkpoint now at %llu", lastSeq);
                _lastSequence = lastSeq;
                _replicator->updatePushCheckpoint(_lastSequence);
            }
        }
    }


    ReplActor::ActivityLevel Pusher::computeActivityLevel() const {
        logDebug("caughtUp=%d, changeLists=%u, revsInFlight=%u, awaitingReply=%u, revsToSend=%zu, pendingSequences=%zu", _caughtUp, _changeListsInFlight, _revisionsInFlight, _revisionBytesAwaitingReply, _revsToSend.size(), _pendingSequences.size());
        if (ReplActor::computeActivityLevel() == kC4Busy
                || (_started && !_caughtUp)
                || _changeListsInFlight > 0
                || _revisionsInFlight > 0
                || !_revsToSend.empty()
                || !_pendingSequences.empty()) {
            return kC4Busy;
        } else if (_options.push == kC4Continuous || isOpenServer()) {
            return kC4Idle;
        } else {
            return kC4Stopped;
        }
    }

    void Pusher::activityLevelChanged(ActivityLevel level) {
        _replicator->taskChangedActivityLevel(this, level);
    }

} }
