//
//  Puller.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Puller.hh"
#include "DBActor.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {


    Puller::Puller(Connection *connection, Replicator *replicator, DBActor *dbActor, Options options)
    :ReplActor(connection, options, string("Pull:") + connection->name())
    ,_replicator(replicator)
    ,_dbActor(dbActor)
    {
        registerHandler("changes",&Puller::handleChanges);
        registerHandler("rev",    &Puller::handleRev);
    }


    // Starting an active pull.
    void Puller::start(std::string sinceSequence) {
        _lastSequence = sinceSequence;
        log("Starting pull from remote seq %s", _lastSequence.c_str());

        MessageBuilder msg("subChanges"_sl);
        msg.noreply = true;
        if (!_lastSequence.empty())
            msg["since"_sl] = _lastSequence;
        if (_options.continuous)
            msg["continuous"_sl] = "true"_sl;
        sendRequest(msg);
    }




    // Handles an incoming "changes" message
    void Puller::handleChanges(Retained<MessageIn> req) {
        log("Handling 'changes' message");
        auto changes = req->JSONBody().asArray();
        if (!changes) {
            warn("Invalid body of 'changes' message");
            req->respondWithError("BLIP"_sl, 400);
            return;
        }

        if (changes.empty()) {
            // Empty array indicates we've caught up.
            log("Caught up with remote changes");
            _caughtUp = true;

            MessageBuilder reply(req);
            req->respond(reply);
        } else if (req->noReply()) {
            warn("Got pointless noreply 'changes' message");
        } else {
            // Pass the buck to the DBAgent so it can find the missing revs & request them:
            ++_pendingCallbacks;
            _dbActor->findOrRequestRevs(req, asynchronize([this](vector<alloc_slice> requests) {
                _requestedSequences.insert(requests.begin(), requests.end());
                log("Now waiting on %zu revisions", _requestedSequences.size());
                --_pendingCallbacks;
            }));
        }
    }


    // Handles an incoming "rev" message, which contains a revision body to insert
    void Puller::handleRev(Retained<MessageIn> msg) {
        Rev rev;
        rev.docID = msg->property("id"_sl);
        rev.revID = msg->property("rev"_sl);
        rev.deleted = !!msg->property("del"_sl);
        slice history = msg->property("history"_sl);
        alloc_slice sequence(msg->property("sequence"_sl));

        log("Received revision '%.*s' #%.*s (seq '%.*s')",
            SPLAT(rev.docID), SPLAT(rev.revID), SPLAT(sequence));
        if (rev.docID.size == 0 || rev.revID.size == 0) {
            warn("Got invalid revision");
            msg->respondWithError("BLIP"_sl, 400);
            return;
        }
        if (_options.pull && !sequence) {
            warn("Missing sequence in 'rev' message for active puller");
            msg->respondWithError("BLIP"_sl, 400);
            return;
        }

        FLError err;
        alloc_slice fleeceBody = Encoder::convertJSON(msg->body(), &err);
        if (!fleeceBody) {
            gotError(C4Error{FleeceDomain, err});
            return;
        }

        function<void(C4Error)> onInserted;
        if (!msg->noReply() || _options.pull) {
            ++_pendingCallbacks;
            onInserted = asynchronize([=](C4Error err) {
                if (err.code) {
                    if (!msg->noReply())
                        msg->respondWithError("LiteCore"_sl, err.code);      //TODO: Proper error domain
                } else {
                    // Finally, the revision has been added! Check it off:
                    if (_options.pull) {
                        _requestedSequences.erase(sequence);
                        log("Inserted rev; waiting on %zu more", _requestedSequences.size());
                    }
                    if (!msg->noReply()) {
                        MessageBuilder response(msg);
                        msg->respond(response);
                    }
                }
                --_pendingCallbacks;
            });
        }

        _dbActor->insertRevision(rev, history, fleeceBody, onInserted);
    }


    // Called after every event; updates busy status & detects when I'm done
    void Puller::afterEvent() {
        bool busy = !_caughtUp || !_requestedSequences.empty() || _pendingCallbacks > 0
                        || eventCount() > 1;
        setBusy(busy);

        if (!busy && !(_options.pull && _options.continuous))
            _replicator->taskComplete(false);
    }


} }
