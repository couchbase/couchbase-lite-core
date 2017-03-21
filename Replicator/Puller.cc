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

    static bool hasUnderscoredProperties(Dict);
    alloc_slice stripUnderscoredProperties(Dict);


    Puller::Puller(Connection *connection, Replicator *replicator, DBActor *dbActor, Options options)
    :ReplActor(connection, replicator, options, "Pull")
    ,_dbActor(dbActor)
    {
        registerHandler("changes",&Puller::handleChanges);
        registerHandler("rev",    &Puller::handleRev);
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
                req->respondWithError("BLIP"_sl, 400);
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
                }
                --_pendingCallbacks;
            }));
        }
    }


    // Handles an incoming "rev" message, which contains a revision body to insert
    void Puller::handleRev(Retained<MessageIn> msg) {
        FLError err;
        alloc_slice fleeceBody = Encoder::convertJSON(msg->body(), &err);
        if (!fleeceBody) {
            gotError(C4Error{FleeceDomain, err});
            return;
        }
        Dict root = Value::fromTrustedData(fleeceBody).asDict();

        auto rev = make_shared<RevToInsert>();
        bool stripUnderscores;
        rev->docID = msg->property("id"_sl);
        if (rev->docID) {
            rev->revID = msg->property("rev"_sl);
            rev->deleted = !!msg->property("deleted"_sl);
            stripUnderscores = hasUnderscoredProperties(root);
        } else {
            // No metadata properties; look inside the JSON:
            rev->docID = (slice)root["_id"_sl].asString();
            rev->revID = (slice)root["_rev"_sl].asString();
            rev->deleted = root["_deleted"].asBool();
            stripUnderscores = true;
        }
        rev->historyBuf = msg->property("history"_sl);
        alloc_slice sequence(msg->property("sequence"_sl));

        logVerbose("Received revision '%.*s' #%.*s (seq '%.*s')",
                   SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(sequence));
        if (rev->docID.size == 0 || rev->revID.size == 0) {
            warn("Got invalid revision");
            msg->respondWithError("BLIP"_sl, 400);
            return;
        }
        if (nonPassive() && !sequence) {
            warn("Missing sequence in 'rev' message for active puller");
            msg->respondWithError("BLIP"_sl, 400);
            return;
        }

        if (stripUnderscores)
            fleeceBody = stripUnderscoredProperties(root);
        rev->body = fleeceBody;

        function<void(C4Error)> onInserted;
        if (!msg->noReply() || nonPassive()) {
            ++_pendingCallbacks;
            rev->onInserted = asynchronize([this,msg,sequence](C4Error err) {
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

        _dbActor->insertRevision(rev);
    }


    // Records that a sequence has been successfully pushed.
    void Puller::markComplete(const alloc_slice &sequence) {
        if (nonPassive()) {
            if (_requestedSequences.remove(sequence)) {
                _lastSequence = _requestedSequences.since();
                logVerbose("Checkpoint now at %.*s", SPLAT(_lastSequence));
                _replicator->updatePullCheckpoint(_lastSequence);
            }
        }
    }

    
    ReplActor::ActivityLevel Puller::computeActivityLevel() const {
        if (ReplActor::computeActivityLevel() == kC4Busy
                || (!_caughtUp && nonPassive())
                || !_requestedSequences.empty()
                || _pendingCallbacks > 0) {
            return kC4Busy;
        } else if (_options.pull == kC4Continuous || isOpenServer()) {
            return kC4Idle;
        } else {
            return kC4Stopped;
        }
    }

    void Puller::activityLevelChanged(ActivityLevel level) {
        _replicator->taskChangedActivityLevel(this, level);
    }


    // Returns true if a Fleece Dict contains any keys that begin with an underscore.
    static bool hasUnderscoredProperties(Dict root) {
        for (Dict::iterator i(root); i; ++i) {
            auto key = slice(i.keyString());
            if (key.size > 0 && key[0] == '_')
                return true;
        }
        return false;
    }


    // Encodes a Dict, skipping top-level properties whose names begin with an underscore.
    alloc_slice stripUnderscoredProperties(Dict root) {
        Encoder e;
        e.beginDict(root.count());
        for (Dict::iterator i(root); i; ++i) {
            auto key = slice(i.keyString());
            if (key.size > 0 && key[0] == '_')
                continue;
            e.writeKey(key);
            e.writeValue(i.value());
        }
        e.endDict();
        return e.finish();
    }


} }
