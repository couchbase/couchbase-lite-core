//
//  Pusher.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Pusher.hh"
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {

    Pusher::Pusher(Replicator *replicator, bool continuous, C4SequenceNumber sinceSequence)
    :_replicator(replicator)
    ,_continuous(continuous)
    ,_lastSequence(sinceSequence)
    {
        _lastSequenceRequested = _lastSequenceSent = _lastSequence;
    }


    void Pusher::start() {
        sendMoreChanges();
    }


    void Pusher::gotError(const MessageIn* msg) {
        // TODO
//        LogToAt(SyncLog, Error, "Got error response: %.*s %d",
//                SPLAT(msg->errorDomain()), msg->errorCode());
    }

    void Pusher::gotError(C4Error err) {
        // TODO
        LogToAt(SyncLog, Error, "Got error response: %d/%d", err.domain, err.code);
    }
    
    void Pusher::sendMoreChanges() {
        if (_changeListsInFlight < kMaxChangeListsInFlight) {
            ++_changeListsInFlight;
            _replicator->dbGetChanges(_lastSequenceRequested, _changesBatchSize, _continuous);
            // response will be to call _gotChanges
        }
    }


    void Pusher::_gotChanges(RevList changes, C4Error err) {
        if (err.code)
            return gotError(err);
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
        auto r = _replicator->sendRequest(req);
        if (!req.noreply) {
            onReady(r, [=](MessageIn* reply) {
                // Got response to the 'changes' message:
                --_changeListsInFlight;
                sendMoreChanges();

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
    }


} }
