//
//  Puller.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//
//  https://github.com/couchbase/couchbase-lite-core/wiki/Replication-Protocol

#include "Puller.hh"
#include "DBWorker.hh"
#include "IncomingRev.hh"
#include "StringUtil.hh"
#include "BLIP.hh"
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace repl {

    Puller::Puller(Connection *connection, Replicator *replicator, DBWorker *dbActor, Options options)
    :Worker(connection, replicator, options, "Pull")
    ,_dbActor(dbActor)
    {
        registerHandler("changes",&Puller::handleChanges);
        registerHandler("rev",    &Puller::handleRev);
        _spareIncomingRevs.reserve(kMaxSpareIncomingRevs);
        _skipDeleted = _options.skipDeleted();
    }


    // Starting an active pull.
    void Puller::_start(alloc_slice sinceSequence) {
        _lastSequence = sinceSequence;
        _requestedSequences.clear(sinceSequence);
        log("Starting pull from remote seq %.*s", SPLAT(_lastSequence));

        MessageBuilder msg("subChanges"_sl);
        if (_lastSequence)
            msg["since"_sl] = _lastSequence;
        if (_options.pull == kC4Continuous)
            msg["continuous"_sl] = "true"_sl;
        msg["batch"_sl] = kChangesBatchSize;

        if (_skipDeleted)
            msg["activeOnly"_sl] = "true"_sl;

        auto channels = _options.channels();
        if (channels) {
            stringstream value;
            unsigned n = 0;
            for (Array::iterator i(channels); i; ++i) {
                slice name = i.value().asString();
                if (name) {
                    if (n++)
                         value << ",";
                    value << name.asString();
                }
            }
            msg["filter"_sl] = "sync_gateway/bychannel"_sl;
            msg["channels"_sl] = value.str();
        } else {
            slice filter = _options.filter();
            if (filter) {
                msg["filter"_sl] = filter;
                for (Dict::iterator i(_options.filterParams()); i; ++i)
                    msg[i.keyString()] = i.value().asString();
            }
        }

        auto docIDs = _options.docIDs();
        if (docIDs) {
            auto &enc = msg.jsonBody();
            enc.beginDict();
            enc.writeKey("docIDs"_sl);
            enc.writeValue(docIDs);
            enc.endDict();
        }
        
        sendRequest(msg, asynchronize([=](blip::MessageProgress progress) {
            //... After request is sent:
            if (progress.reply && progress.reply->isError()) {
                gotError(progress.reply);
                _fatalError = true;
            }
        }));
    }


    // Handles an incoming "changes" message
    void Puller::handleChanges(Retained<MessageIn> req) {
        logVerbose("Handling 'changes' message");
        auto changes = req->JSONBody().asArray();
        if (!changes && req->body() != "null"_sl) {
            warn("Invalid body of 'changes' message");
            req->respondWithError({"BLIP"_sl, 400, "Invalid JSON body"_sl});
            return;
        }

        if (changes.empty()) {
            // Empty array indicates we've caught up.
            log("Caught up with remote changes");
            _caughtUp = true;
            _skipDeleted = false;
            req->respond();
        } else if (req->noReply()) {
            warn("Got pointless noreply 'changes' message");
        } else {
            // Pass the buck to the DBWorker so it can find the missing revs & request them:
            ++_pendingCallbacks;
            _dbActor->findOrRequestRevs(req, asynchronize([this,req,changes](vector<bool> which) {
                --_pendingCallbacks;
                if (nonPassive()) {
                    // Keep track of which remote sequences I just requested:
                    for (size_t i = 0; i < which.size(); ++i) {
                        if (which[i]) {
                            auto change = changes[(unsigned)i].asArray();
                            uint64_t bodySize = max(change[4].asUnsigned(), (uint64_t)1);
                            alloc_slice sequence(change[0].toString()); //FIX: Should quote strings
                            if (sequence)
                                _requestedSequences.add(sequence, bodySize);
                            else
                                warn("Empty/invalid sequence in 'changes' message");
                            addProgress({0, bodySize});
                        }
                    }
                    logVerbose("Now waiting on %zu revisions", _requestedSequences.size());
                }
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


    // Records that a sequence has been successfully pulled.
    void Puller::_revWasHandled(Retained<IncomingRev> inc, alloc_slice sequence, bool complete) {
        --_pendingCallbacks;
        if (complete && nonPassive()) {
            bool wasEarliest;
            uint64_t bodySize;
            _requestedSequences.remove(sequence, wasEarliest, bodySize);
            if (wasEarliest) {
                _lastSequence = _requestedSequences.since();
                logVerbose("Checkpoint now at %.*s", SPLAT(_lastSequence));
                if (replicator())
                    replicator()->updatePullCheckpoint(_lastSequence);
            }
            addProgress({bodySize, 0});
        }

        if (_spareIncomingRevs.size() < kMaxSpareIncomingRevs) {
            logDebug("Recycling IncomingRev<%p>", inc.get());
            _spareIncomingRevs.push_back(inc);
        }
    }


    void Puller::_childChangedStatus(Worker *task, Status status) {
        // Combine the IncomingRev's progress into mine:
        addProgress(status.progressDelta);
    }

    
    Worker::ActivityLevel Puller::computeActivityLevel() const {
        if (_fatalError) {
            return kC4Stopped;
        } else if (Worker::computeActivityLevel() == kC4Busy
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
