//
// Puller.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//  https://github.com/couchbase/couchbase-lite-core/wiki/Replication-Protocol

#include "Puller.hh"
#include "DBWorker.hh"
#include "IncomingRev.hh"
#include "Error.hh"
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
        registerHandler("changes",          &Puller::handleChanges);
        registerHandler("proposeChanges",   &Puller::handleChanges);
        registerHandler("rev",              &Puller::handleRev);
        registerHandler("norev",            &Puller::handleNoRev);
        _spareIncomingRevs.reserve(kMaxActiveIncomingRevs);
        _skipDeleted = _options.skipDeleted();
        if (nonPassive() && options.noIncomingConflicts())
            warn("noIncomingConflicts mode is not compatible with active pull replications!");
    }


    // Starting an active pull.
    void Puller::_start(alloc_slice sinceSequence) {
        _lastSequence = sinceSequence;
        _missingSequences.clear(sinceSequence);
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
        
        sendRequest(msg, [=](blip::MessageProgress progress) {
            //... After request is sent:
            if (progress.reply && progress.reply->isError()) {
                gotError(progress.reply);
                _fatalError = true;
            }
        });
    }


#pragma mark - INCOMING CHANGE LISTS:


    // Receiving an incoming "changes" (or "proposeChanges") message
    void Puller::handleChanges(Retained<MessageIn> req) {
        logVerbose("Received '%.*s' REQ#%llu (%u pending revs)",
                   SPLAT(req->property("Profile"_sl)), req->number(), _pendingRevMessages);
        _waitingChangesMessages.push_back(move(req));
        handleMoreChanges();
    }


    // Process waiting "changes" messages if not throttled:
    void Puller::handleMoreChanges() {
        while (!_waitingChangesMessages.empty() && !_waitingForChangesCallback
               && _pendingRevMessages + kChangesBatchSize <= kMaxActiveIncomingRevs) {
            auto req = _waitingChangesMessages.front();
            _waitingChangesMessages.pop_front();
            handleChangesNow(req);
        }
    }


    // Actually handle a "changes" message:
    void Puller::handleChangesNow(Retained<MessageIn> req) {
        slice reqType = req->property("Profile"_sl);
        bool proposed = (reqType == "proposeChanges"_sl);
        logVerbose("Handling '%.*s' message REQ#%llu", SPLAT(reqType), req->number());

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
        } else if (_options.noIncomingConflicts() && !proposed) {
            // In conflict-free mode the protocol requires the pusher send "proposeChanges" instead
            req->respondWithError({"BLIP"_sl, 409});
        } else {
            // Pass the buck to the DBWorker so it can find the missing revs & request them:
            DebugAssert(!_waitingForChangesCallback);
            _waitingForChangesCallback = true;
            _dbActor->findOrRequestRevs(req, asynchronize([this,req,changes](vector<bool> which) {
                // Callback, after response message sent:
                _waitingForChangesCallback = false;
                for (size_t i = 0; i < which.size(); ++i) {
                    bool requesting = (which[i]);
                    if (nonPassive()) {
                        // Add sequence to _missingSequences:
                        auto change = changes[(unsigned)i].asArray();
                        alloc_slice sequence(change[0].toJSON());
                        uint64_t bodySize = requesting ? max(change[4].asUnsigned(), (uint64_t)1) : 0;
                        if (sequence)
                            _missingSequences.add(sequence, bodySize);
                        else
                            warn("Empty/invalid sequence in 'changes' message");
                        addProgress({0, bodySize});
                        if (!requesting)
                            completedSequence(sequence); // Not requesting, just update checkpoint
                    }
                    if (requesting) {
                        increment(_pendingRevMessages);
                        // now awaiting a handleRev call...
                    }
                }
                if (nonPassive()) {
                    logVerbose("Now waiting for %u 'rev' messages; %zu known sequences pending",
                               _pendingRevMessages, _missingSequences.size());
                }
                handleMoreChanges();  // because _waitingForChangesCallback changed
            }));
        }
    }


#pragma mark - INCOMING REVS:


    // Received an incoming "rev" message, which contains a revision body to insert
    void Puller::handleRev(Retained<MessageIn> msg) {
        if (_activeIncomingRevs < kMaxActiveIncomingRevs) {
            startIncomingRev(msg);
        } else {
            logVerbose("Delaying handling 'rev' message for '%.*s' [%zu waiting]",
                       SPLAT(msg->property("id"_sl)), _waitingRevMessages.size()+1);//TEMP
            _waitingRevMessages.push_back(move(msg));
        }
    }


    void Puller::handleNoRev(Retained<MessageIn> msg) {
        decrement(_pendingRevMessages);
        handleMoreChanges();
        if (!msg->noReply()) {
            MessageBuilder response(msg);
            msg->respond(response);
        }
    }


    // Actually process an incoming "rev" now:
    void Puller::startIncomingRev(MessageIn *msg) {
        decrement(_pendingRevMessages);
        increment(_activeIncomingRevs);
        Retained<IncomingRev> inc;
        if (_spareIncomingRevs.empty()) {
            inc = new IncomingRev(this, _dbActor);
        } else {
            inc = _spareIncomingRevs.back();
            _spareIncomingRevs.pop_back();
        }
        inc->handleRev(msg);  // ... will call _revWasHandled when it's finished
        handleMoreChanges();
    }


    void Puller::revWasHandled(IncomingRev *inc,
                               const alloc_slice &docID,
                               slice sequence,
                               bool successful)
    {
        enqueue(&Puller::_revWasHandled, retained(inc), docID, alloc_slice(sequence), successful);
    }


    // Callback from an IncomingRev when it's finished (either added to db, or failed)
    void Puller::_revWasHandled(Retained<IncomingRev> inc,
                                alloc_slice docID,
                                alloc_slice sequence,
                                bool successful)
    {
        if (successful && nonPassive()) {
            completedSequence(sequence);
            finishedDocument(docID, false);
        }

        _spareIncomingRevs.push_back(inc);

        decrement(_activeIncomingRevs);
        if (_activeIncomingRevs < kMaxActiveIncomingRevs && !_waitingRevMessages.empty()) {
            auto msg = _waitingRevMessages.front();
            _waitingRevMessages.pop_front();
            startIncomingRev(msg);
        } else {
            handleMoreChanges();
        }
    }


    // Records that a sequence has been successfully pulled.
    void Puller::completedSequence(alloc_slice sequence) {
        bool wasEarliest;
        uint64_t bodySize;
        _missingSequences.remove(sequence, wasEarliest, bodySize);
        if (wasEarliest) {
            _lastSequence = _missingSequences.since();
            logVerbose("Checkpoint now at %.*s", SPLAT(_lastSequence));
            if (replicator())
                replicator()->updatePullCheckpoint(_lastSequence);
        }
        addProgress({bodySize, 0});
    }


#pragma mark - STATUS / PROGRESS:


    void Puller::_childChangedStatus(Worker *task, Status status) {
        // Combine the IncomingRev's progress into mine:
        addProgress(status.progressDelta);
    }

    
    Worker::ActivityLevel Puller::computeActivityLevel() const {
        ActivityLevel level;
        if (_fatalError) {
            level = kC4Stopped;
        } else if (Worker::computeActivityLevel() == kC4Busy
                || (!_caughtUp && nonPassive())
                || _waitingForChangesCallback
                || _pendingRevMessages > 0
                || _activeIncomingRevs > 0) {
            level = kC4Busy;
        } else if (_options.pull == kC4Continuous || isOpenServer()) {
            const_cast<Puller*>(this)->_spareIncomingRevs.clear();
            level = kC4Idle;
        } else {
            level = kC4Stopped;
        }
        if (SyncBusyLog.effectiveLevel() <= LogLevel::Info) {
            log("activityLevel=%-s: pendingResponseCount=%d, _caughtUp=%d, _waitingForChangesCallback=%d, _pendingRevMessages=%u, _activeIncomingRevs=%u",
                kC4ReplicatorActivityLevelNames[level],
                pendingResponseCount(), _caughtUp, _waitingForChangesCallback,
                _pendingRevMessages, _activeIncomingRevs);
        }
        return level;
    }


} }
