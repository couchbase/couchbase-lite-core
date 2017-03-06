//
//  Puller.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Puller.hh"
#include "DBActor.hh"
#include "StringUtil.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {


    Puller::Puller(Connection *connection, Replicator *replicator, DBActor *dbActor, Options options)
    :ReplActor(connection, options, "Pull")
    ,_replicator(replicator)
    ,_dbActor(dbActor)
    {
        registerHandler("changes",&Puller::handleChanges);
        registerHandler("rev",    &Puller::handleRev);
    }


    // Starting an active pull.
    void Puller::start(alloc_slice sinceSequence) {
        _lastSequence = sinceSequence;
        _requestedSequences.clear(sinceSequence);
        log("Starting pull from remote seq %.*s", SPLAT(_lastSequence));

        MessageBuilder msg("subChanges"_sl);
        msg.noreply = true;
        if (_lastSequence)
            msg["since"_sl] = _lastSequence;
        if (_options.pull == kC4Continuous)
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
            // Pass the buck to the DBActor so it can find the missing revs & request them:
            ++_pendingCallbacks;
            _dbActor->findOrRequestRevs(req, asynchronize([this](vector<alloc_slice> requests) {
                if (active()) {
                    for (auto &r : requests)
                        _requestedSequences.add(r);
                    log("Now waiting on %zu revisions", _requestedSequences.size());
                }
                --_pendingCallbacks;
            }));
        }
    }


    // Handles an incoming "rev" message, which contains a revision body to insert
    void Puller::handleRev(Retained<MessageIn> msg) {
        Rev rev;
        rev.docID = msg->property("id"_sl);
        rev.revID = msg->property("rev"_sl);
        bool deleted = !!msg->property("del"_sl);
        slice history = msg->property("history"_sl);
        alloc_slice sequence(msg->property("sequence"_sl));

        log("Received revision '%.*s' #%.*s (seq '%.*s')",
            SPLAT(rev.docID), SPLAT(rev.revID), SPLAT(sequence));
        if (rev.docID.size == 0 || rev.revID.size == 0) {
            warn("Got invalid revision");
            msg->respondWithError("BLIP"_sl, 400);
            return;
        }
        if (active() && !sequence) {
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
        if (!msg->noReply() || active()) {
            ++_pendingCallbacks;
            onInserted = asynchronize([=](C4Error err) {
                if (err.code) {
                    if (!msg->noReply())
                        msg->respondWithError("LiteCore"_sl, err.code);      //TODO: Proper error domain
                } else {
                    // Finally, the revision has been added! Check it off:
                    markComplete(sequence);
                    if (!msg->noReply()) {
                        MessageBuilder response(msg);
                        msg->respond(response);
                    }
                }
                --_pendingCallbacks;
            });
        }

        _dbActor->insertRevision(rev, deleted, history, fleeceBody, onInserted);
    }


    // Records that a sequence has been successfully pushed.
    void Puller::markComplete(const alloc_slice &sequence) {
        if (active()) {
            if (_requestedSequences.remove(sequence)) {
                _lastSequence = _requestedSequences.since();
                logVerbose("Checkpoint now at %.*s", SPLAT(_lastSequence));
                _replicator->updatePullCheckpoint(_lastSequence);
            }
        }
    }

    
    bool Puller::isBusy() const {
        return ReplActor::isBusy()
            || (!_caughtUp && active())
            || !_requestedSequences.empty()
            || _pendingCallbacks > 0;
    }


    // Called after every event; updates busy status & detects when I'm done
    void Puller::afterEvent() {
        if (!isBusy() && !(active() && _options.pull == kC4Continuous) && isOpenClient()) {
            log("Finished!");
            _replicator->taskComplete(false);
        }
    }


} }
