//
//  Pusher.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Pusher.hh"
#include "JSONEncoder.hh"

using namespace fleece;

namespace litecore { namespace repl {

    void Pusher::start() {
        sendChangesSince(_lastSequence);
    }


    void Pusher::sendChangesSince(sequence_t since) {
        auto futureChanges = _replicator->dbGetChanges(since, _changesBatchSize);
        onReady(futureChanges, [=](ChangeList cl) {
            if (cl.error.code) {
                return;
            }
            LogTo(SyncLog, "Sending %zu changes since sequence #%llu",
                  cl.changes.size(), since);
            sendChangeList(cl);

            if (_continuous && cl.changes.empty()) {
                // Now go into continuous-push mode, waiting for db changes:
                LogTo(SyncLog, "Now observing database change notifications");
                _replicator->beginObservingChanges();
            }
        });
    }


    void Pusher::_databaseChanged(ChangeList cl) {
        sendChangeList(cl);
    }


    void Pusher::sendChangeList(const ChangeList &cl) {
        // Encode changes as JSON:
        alloc_slice json;
        JSONEncoder enc;
        enc.beginArray();
        for (auto &change : cl.changes) {
            enc.beginArray();
            bool deleted = (change.flags & kRevDeleted) != 0;
            enc << change.sequence << change.docID << change.revID << deleted;
            enc.endArray();
        }
        enc.endArray();
        
        // Send outgoing request:
        MessageBuilder req("changes"_sl);
        req.urgent = kChangeMessagesAreUrgent;
        req.noreply = cl.changes.empty();
        req << enc.extractOutput();
        auto r = _replicator->sendRequest(req);
        if (!req.noreply) {
            onReady(r, [](MessageIn* reply) {
                // Got response to the 'changes' message; see which revs the peer wants:
            });
        }

    }

} }
