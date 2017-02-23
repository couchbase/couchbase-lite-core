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
    { }


    // Begin active push, starting from the next sequence after sinceSequence
    void Pusher::start(C4SequenceNumber sinceSequence, const Replicator::Options &options) {
        _options = options;
        _lastSequenceRead = _lastSequenceSent = _lastSequence = sinceSequence;
        log("Starting push from local seq %llu", _lastSequence+1);
        maybeGetMoreChanges();
    }


    // Request another batch of changes from the db, if there aren't too many in progress
    void Pusher::maybeGetMoreChanges() {
        if (_curSequenceRequested == 0
                && _changeListsInFlight < kMaxChangeListsInFlight && !_caughtUp ) {
            ++_changeListsInFlight;
            _curSequenceRequested = _lastSequenceRead + 1;
            log("Reading %u changes since sequence %llu ...",
                  _changesBatchSize, _lastSequenceRead);
            _dbActor->getChanges(_lastSequenceRead, _changesBatchSize, _options.continuous, this);
            // response will be to call _gotChanges
        }
    }


    // Received a list of changes from the database [initiated in getMoreChanges]
    void Pusher::_gotChanges(RevList changes, C4Error err) {
        if (err.code)
            return gotError(err);
        _curSequenceRequested = 0;
        if (!changes.empty()) {
            _lastSequenceRead = changes.back().sequence;
            log("Read %zu changes: Pusher sending 'changes' with sequences %llu - %llu",
                  changes.size(), changes[0].sequence, _lastSequenceRead);
        }

        // Construct the 'changes' message:
        MessageBuilder req("changes"_sl);
        req.urgent = kChangeMessagesAreUrgent;
        auto &enc = req.jsonBody();
        enc.beginArray();
        for (auto &change : changes) {
            enc.beginArray();
            enc << change.sequence << change.docID << change.revID;
            if (change.deleted)
                enc << true;
            enc.endArray();
        }
        enc.endArray();

        // Send, and asynchronously handle the response:
        auto r = sendRequest(req);
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
                }
                ++index;
            }
        });

        if (changes.size() < _changesBatchSize) {
            log("Caught up, at lastSequence %llu", _lastSequenceRead);
            _caughtUp = true;
            if (_options.push && !_options.continuous) {
                // done
            }
        } else {
            maybeGetMoreChanges();
        }
    }


    void Pusher::sendRevision(const Rev &rev,
                              const vector<string> &ancestorRevIDs,
                              unsigned maxHistory)
    {
        function<void(MessageIn*)> onReply;
        if (_options.push) {
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
        _dbActor->sendRevision(rev, ancestorRevIDs, maxHistory, onReply);
    }


    void Pusher::markComplete(C4SequenceNumber sequence) {
        if (sequence > _lastSequence) {
            _lastSequence = sequence;
            logVerbose("Checkpoint now at %llu", _lastSequence);
        }
    }


    void Pusher::afterEvent() {
        bool busy = (_changeListsInFlight > 0  || _revisionsInFlight > 0
                     || !_caughtUp || eventCount() > 1);
        setBusy(busy);

        if (!busy && _caughtUp)
            _replicator->taskComplete(true);
    }


} }
