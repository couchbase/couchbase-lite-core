//
//  Pusher.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Pusher.hh"
#include <algorithm>

#define SPLAT(S)    (int)(S).size, (S).buf      // Use with %.* formatter

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {

    Pusher::Pusher(Replicator *replicator)
    :_replicator(replicator)
    {
        setConnection(replicator->connection());
    }


    void Pusher::start(C4SequenceNumber sinceSequence, bool continuous) {
        _continuous = continuous;
        _lastSequenceRequested = _lastSequenceSent = _lastSequence = sinceSequence;
        LogTo(SyncLog, "Starting push from local seq %llu", _lastSequence);
        getMoreChanges();
    }


    // Request another batch of changes from the db, if there aren't too many in progress
    void Pusher::getMoreChanges() {
        if (_changeListsInFlight < kMaxChangeListsInFlight) {
            ++_changeListsInFlight;
            _replicator->dbGetChanges(_lastSequenceRequested, _changesBatchSize, _continuous);
            // response will be to call _gotChanges
        }
    }


    // Received a list of changes from the database [initiated in getMoreChanges]
    void Pusher::_gotChanges(RevList changes, C4Error err) {
        if (err.code)
            return gotError(err);
        if (changes.empty()) {
            LogTo(SyncLog, "Pusher has no more changes");
        } else {
            LogTo(SyncLog, "Pusher sending 'changes' with sequences %llu - %llu",
                  changes[0].sequence, changes.back().sequence);
        }

        // Construct the 'changes' message:
        MessageBuilder req("changes"_sl);
        req.urgent = kChangeMessagesAreUrgent;
        auto &enc = req.jsonBody();
        enc.beginArray();
        for (auto &change : changes) {
            enc.beginArray();
            bool deleted = (change.flags & kRevDeleted) != 0;
            enc << change.sequence << change.docID << change.revID << deleted;
            enc.endArray();
            _lastSequenceRequested = max(_lastSequenceRequested, change.sequence);
        }
        enc.endArray();

        // Send, and asynchronously handle the response:
        auto r = _replicator->sendRequest(req);
        onReady(r, [this, changes](MessageIn* reply) {
            // Got response to the 'changes' message:
            --_changeListsInFlight;
            if (!changes.empty())
                getMoreChanges();

            if (reply->isError())
                return gotError(reply);

            // The response contains an array that, for each change in the outgoing message,
            // contains either a list of known ancestors, or null/false/0 if not interested.
            auto requests = reply->JSONBody().asArray();
            if (!requests.empty()) {
                int maxHistory = (int)max(0l, reply->intProperty("maxHistory"_sl));
                unsigned index = 0;
                for (Value item : requests) {
                    Array ancestors = item.asArray();
                    if (ancestors) {
                        vector<string> revIDs;
                        revIDs.reserve(ancestors.count());
                        for (Value a : ancestors) {
                            slice revid = a.asstring();
                            if (revid)
                                revIDs.push_back(revid.asString());
                        }
                        _replicator->dbSendRevision(changes[index], revIDs, maxHistory);
                    }
                    ++index;
                }
            }
        });
    }


} }
