//
//  Pusher.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Pusher.hh"
#include "DBActor.hh"
#include <algorithm>

#define SPLAT(S)    (int)(S).size, (S).buf      // Use with %.* formatter

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {

    Pusher::Pusher(Connection *connection, Replicator *replicator, DBActor *dbActor, Options options)
    :ReplActor(connection, options, string("Push:") + connection->name())
    ,_replicator(replicator)
    ,_dbActor(dbActor)
    {
        registerHandler("subChanges",       &Pusher::handleSubChanges);
    }


    // Begins active push, starting from the next sequence after sinceSequence
    void Pusher::start(C4SequenceNumber sinceSequence) {
        log("Starting %spush from local seq %llu",
            (_options.continuous ? "continuous " : ""), _lastSequence+1);
        _pendingSequences.clear(sinceSequence);
        startSending(sinceSequence);
    }


    // Handles an incoming "subChanges" message: starts passive push (i.e. the peer is pulling).
    void Pusher::handleSubChanges(Retained<MessageIn> req) {
        if (_options.push) {
            warn("Ignoring 'subChanges' request from peer; I'm already pushing");
            req->respondWithError("LiteCore"_sl, kC4ErrorConflict,
                                  "I'm already pushing"_sl);     //TODO: Proper error code
            return;
        }
        auto since = max(req->intProperty("since"_sl), 0l);
        _options.continuous = req->boolProperty("continuous"_sl);
        log("Peer is pulling %schanges from seq %llu",
            (_options.continuous ? "continuous " : ""), _lastSequence);
        startSending(since);
    }


    // Starts active or passive push from the given sequence number.
    void Pusher::startSending(C4SequenceNumber sinceSequence) {
        _lastSequenceRead = _lastSequenceSent = _lastSequence = sinceSequence;
        maybeGetMoreChanges();
    }


    // Request another batch of changes from the db, if there aren't too many in progress
    void Pusher::maybeGetMoreChanges() {
        if (!_gettingChanges && _changeListsInFlight < kMaxChangeListsInFlight && !_caughtUp ) {
            _gettingChanges = true;
            ++_changeListsInFlight;
            log("Reading %u changes since sequence %llu ...", _changesBatchSize, _lastSequenceRead);
            _dbActor->getChanges(_lastSequenceRead, _changesBatchSize, _options.continuous, this);
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
        auto r = sendChanges(changes);
        onReady(r, [this, changes](MessageIn* reply) {
            // Got response to the 'changes' message:
            --_changeListsInFlight;
            maybeGetMoreChanges();

            if (reply->isError())
                return gotError(reply);

            // The response contains an array that, for each change in the outgoing message,
            // contains either a list of known ancestors, or null/false/0 if not interested.
            int maxHistory = (int)max(0l, reply->intProperty("maxHistory"_sl));
            vector<string> ancestorRevIDs;
            auto requests = reply->JSONBody().asArray();

            unsigned index = 0;
            for (auto &change : changes) {
                Array ancestorArray = requests[index].asArray();
                if (ancestorArray) {
                    ancestorRevIDs.clear();
                    ancestorRevIDs.reserve(ancestorArray.count());
                    for (Value a : ancestorArray) {
                        slice revid = a.asstring();
                        if (revid)
                            ancestorRevIDs.push_back(revid.asString());
                    }
                    sendRevision(change, ancestorRevIDs, maxHistory);
                } else {
                    markComplete(change.sequence);  // unwanted, so we're done with it
                }
                ++index;
            }
        });

        if (changes.size() < _changesBatchSize) {
            if (!_caughtUp) {
                log("Caught up, at lastSequence %llu", _lastSequenceRead);
                _caughtUp = true;
                if (changes.size() > 0 && !_options.push) {
                    // The protocol says catching up is signaled by an empty changes list:
                    sendChanges(RevList());
                }
            }
        } else {
            maybeGetMoreChanges();
        }
    }


    // Subroutine of _gotChanges that actually sends a "changes" message:
    blip::FutureResponse Pusher::sendChanges(const RevList &changes) {
        MessageBuilder req("changes"_sl);
        req.urgent = kChangeMessagesAreUrgent;
        auto &enc = req.jsonBody();
        enc.beginArray();
        for (auto &change : changes) {
            if (_options.push)
                _pendingSequences.add(change.sequence);
            enc.beginArray();
            enc << change.sequence << change.docID << change.revID;
            if (change.deleted)
                enc << true;
            enc.endArray();
        }
        enc.endArray();
        return sendRequest(req);
    }

    
    // Subroutine of _gotChanges that sends a "rev" message containing a revision body.
    void Pusher::sendRevision(const Rev &rev,
                              const vector<string> &ancestorRevIDs,
                              unsigned maxHistory)
    {
        function<void(MessageIn*)> onReply;
        if (_options.push) {
            // Callback for after the peer receives the "rev" message:
            ++_revisionsInFlight;
            onReply = asynchronize([=](Retained<MessageIn> reply) {
                --_revisionsInFlight;
                if (reply->isError())
                    gotError(reply);
                else {
                    log("Completed rev %.*s #%.*s (seq %llu)",
                          SPLAT(rev.docID), SPLAT(rev.revID), rev.sequence);
                    markComplete(rev.sequence);
                }
            });
        }
        // Tell the DBAgent to actually read from the DB and send the message:
        _dbActor->sendRevision(rev, ancestorRevIDs, maxHistory, onReply);
    }


    // Records that a sequence has been successfully pushed.
    void Pusher::markComplete(C4SequenceNumber sequence) {
        if (_options.push) {
            _pendingSequences.remove(sequence);
            auto firstPending = _pendingSequences.first();
            auto lastSeq = firstPending ? firstPending - 1 : _pendingSequences.maxEver();
            if (lastSeq > _lastSequence) {
                _lastSequence = lastSeq;
                //_replicator->setPushSequence(_lastSequence);
                logVerbose("Checkpoint now at %llu", _lastSequence);
                _replicator->updatePushCheckpoint(_lastSequence);
            }
        }
    }


    bool Pusher::isBusy() const {
        return ReplActor::isBusy()
            || _changeListsInFlight > 0
            || _revisionsInFlight > 0
            || !_pendingSequences.empty();
    }


    // Called after every event; updates busy status & detects when I'm done
    void Pusher::afterEvent() {
        if (!isBusy() && _caughtUp && !(_options.push && _options.continuous)) {
            log("Finished!");
            _replicator->taskComplete(true);
        }
    }

} }
