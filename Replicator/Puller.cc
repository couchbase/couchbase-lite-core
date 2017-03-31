//
//  Puller.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Puller.hh"
#include "DBActor.hh"
#include "IncomingRev.hh"
#include "StringUtil.hh"

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {

    Puller::Puller(Connection *connection, Replicator *replicator, DBActor *dbActor, Options options)
    :ReplActor(connection, replicator, options, "Pull")
    ,_dbActor(dbActor)
    {
        registerHandler("changes",&Puller::handleChanges);
        registerHandler("rev",    &Puller::handleRev);
        _spareIncomingRevs.reserve(kMaxSpareIncomingRevs);
    }


    // Starting an active pull.
    void Puller::_start(alloc_slice sinceSequence) {
        _lastSequence = sinceSequence;
        _requestedSequences.clear(sinceSequence);
        log("Starting pull from remote seq %.*s", SPLAT(_lastSequence));

        MessageBuilder msg("subChanges"_sl);
        msg.noreply = true;
        if (_lastSequence)
            msg["since"_sl] = _lastSequence;
        if (_options.pull == kC4Continuous)
            msg["continuous"_sl] = "true"_sl;
        msg["batch"_sl] = kChangesBatchSize;
        sendRequest(msg);
    }


    // Handles an incoming "changes" message
    void Puller::handleChanges(Retained<MessageIn> req) {
        logVerbose("Handling 'changes' message");
        auto changes = req->JSONBody().asArray();
        if (!changes) {
            warn("Invalid body of 'changes' message");
            if (req->body() != "null"_sl) {       // allow null since SG seems to send it(?)
                req->respondWithError({"BLIP"_sl, 400, "Invalid JSON body"_sl});
                return;
            }
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
                if (nonPassive()) {
                    for (auto &r : requests)
                        _requestedSequences.add(r);
                    log("Now waiting on %zu revisions", _requestedSequences.size());
                    addProgress({0, requests.size()});
                }
                --_pendingCallbacks;
            }));
        }
    }


    // Handles an incoming "rev" message, which contains a revision body to insert
    void Puller::handleRev(Retained<MessageIn> msg) {
        Retained<IncomingRev> inc;
        if (_spareIncomingRevs.empty()) {
            inc = new IncomingRev(this, _dbActor);
            logDebug("Created IncomingRev<%p>", inc.get());
        } else {
            inc = _spareIncomingRevs.back();
            _spareIncomingRevs.pop_back();
            logDebug("Re-using IncomingRev<%p>", inc.get());
        }
        inc->handleRev(msg);
        ++_pendingCallbacks;
    }


    void Puller::revWasHandled(IncomingRev *inc, slice sequence, bool complete) {
        enqueue(&Puller::_revWasHandled, retained(inc),
                alloc_slice(sequence), complete);
    }


    // Records that a sequence has been successfully pushed.
    void Puller::_revWasHandled(Retained<IncomingRev> inc, alloc_slice sequence, bool complete) {
        --_pendingCallbacks;
        if (complete && nonPassive()) {
            addProgress({1, 0});
            if (_requestedSequences.remove(sequence)) {
                _lastSequence = _requestedSequences.since();
                logVerbose("Checkpoint now at %.*s", SPLAT(_lastSequence));
                _replicator->updatePullCheckpoint(_lastSequence);
            }
        }

        if (_spareIncomingRevs.size() < kMaxSpareIncomingRevs) {
            logDebug("Recycling IncomingRev<%p>", inc.get());
            _spareIncomingRevs.push_back(inc);
        }
    }

    
    ReplActor::ActivityLevel Puller::computeActivityLevel() const {
        if (ReplActor::computeActivityLevel() == kC4Busy
                || (!_caughtUp && nonPassive())
                || !_requestedSequences.empty()
                || _pendingCallbacks > 0) {
            return kC4Busy;
        } else if (_options.pull == kC4Continuous || isOpenServer()) {
            const_cast<Puller*>(this)->_spareIncomingRevs.clear();
            return kC4Idle;
        } else {
            return kC4Stopped;
        }
    }


} }
