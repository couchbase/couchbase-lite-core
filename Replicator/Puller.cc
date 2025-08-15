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
#include "Replicator.hh"
#include "RevFinder.hh"
#include "Inserter.hh"
#include "IncomingRev.hh"
#include "DBAccess.hh"
#include "ReplicatorTuning.hh"
#include "Increment.hh"
#include "StringUtil.hh"
#include "Instrumentation.hh"
#include <algorithm>

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore::repl {
    Puller::Puller(Replicator* replicator, CollectionIndex coll)
        : Delegate(replicator, "Pull", coll)
        , _provisionallyHandledRevs(this, "provisionallyHandledRevs", &Puller::_revsWereProvisionallyHandled)
        , _provisionallyHandledRevoked(this, "provisionallyHandledRevoked", &Puller::_revsWereProvisionallyHandled)
        , _returningRevs(this, "returningRevs", &Puller::_revsFinished)
#if __APPLE__
        , _revMailbox(nullptr, "Puller revisions")
#endif
        , _inserter(new Inserter(replicator, coll))
        , _revFinder(new RevFinder(replicator, this, coll)) {
        setParentObjectRef(replicator->getObjectRef());
        replicator->registerWorkerHandler(this, "rev", &Puller::handleRev);
        replicator->registerWorkerHandler(this, "norev", &Puller::handleNoRev);
        _spareIncomingRevs.reserve(tuning::kMaxActiveIncomingRevs);
        _skipDeleted = _options->skipDeleted();
        if ( !passive() && _options->noIncomingConflicts() )
            warn("noIncomingConflicts mode is not compatible with active pull replications!");
    }

    // Starting an active pull.
    void Puller::_start(RemoteSequence sinceSequence) {
        _lastSequence = sinceSequence;
        _missingSequences.clear(sinceSequence);
        alloc_slice sinceStr = _lastSequence.toJSON();
        logInfo("Starting pull from remote seq '%.*s'", SPLAT(sinceStr));

        Signpost::begin(Signpost::blipSent);
        MessageBuilder msg("subChanges"_sl);
        assignCollectionToMsg(msg, collectionIndex());
        if ( sinceStr ) msg["since"_sl] = sinceStr;
        if ( _options->pull(collectionIndex()) == kC4Continuous ) msg["continuous"_sl] = "true"_sl;
        msg["batch"_sl] = tuning::kChangesBatchSize;
#ifdef LITECORE_CPPTEST
        msg["sendReplacementRevs"] = !_revFinder->_disableReplacementRevs;
#else
        msg["sendReplacementRevs"] = tuning::kChangesReplacementRevs;
#endif
        msg["versioning"] = _db->usingVersionVectors() ? "version-vectors" : "rev-trees";
        if ( _skipDeleted ) msg["activeOnly"_sl] = "true"_sl;
        if ( _options->enableAutoPurge() || progressNotificationLevel() > 0 ) {
            msg["revocations"] = "true";  // Enable revocation notification in "changes" (SG 3.0)
            logInfo(R"(msg["revocations"]="true" due to enableAutoPurge()=%d or progressNotificationLevel()=%d > 0)",
                    _options->enableAutoPurge(), progressNotificationLevel());
        }

        auto channels = _options->channels(collectionIndex());
        if ( channels ) {
            stringstream value;
            unsigned     n = 0;
            for ( Array::iterator i(channels); i; ++i ) {
                slice name = i.value().asString();
                if ( name ) {
                    if ( n++ ) value << ",";
                    value << name.asString();
                }
            }
            msg["filter"_sl]   = "sync_gateway/bychannel"_sl;
            msg["channels"_sl] = value.str();
        } else {
            slice filter = _options->filter();
            if ( filter ) {
                msg["filter"_sl] = filter;
                for ( Dict::iterator i(_options->filterParams()); i; ++i ) msg[i.keyString()] = i.value().asString();
            }
        }

        auto docIDs = _options->docIDs(collectionIndex());
        if ( docIDs ) {
            auto& enc = msg.jsonBody();
            enc.beginDict();
            enc.writeKey("docIDs"_sl);
            enc.writeValue(docIDs);
            enc.endDict();
        }

        sendRequest(msg, [this](const blip::MessageProgress& progress) {
            //... After request is sent:
            if ( progress.reply && progress.reply->isError() ) {
                gotError(progress.reply);
                _fatalError = true;
            }
            if ( progress.state == MessageProgress::kComplete ) Signpost::end(Signpost::blipSent);
        });
    }

#pragma mark - INCOMING REVS:

    // Called by the RevFinder to tell the Puller what revs it's requested from the peer.
    void Puller::_expectSequences(vector<RevFinder::ChangeSequence> sequences) {
        for ( auto& change : sequences ) {
            if ( !passive() ) {
                // Add sequence to _missingSequences:
                _missingSequences.add(change.sequence, change.bodySize);
                if ( change.requested() ) addProgress({0, change.bodySize});
                else
                    completedSequence(change.sequence);  // Not requesting, just update checkpoint
            }
            if ( change.requested() ) {
                increment(_pendingRevMessages);
                // now awaiting a handleRev call...
            }
        }
        if ( !passive() ) {
            logVerbose("Now waiting for %u 'rev' messages; %zu known sequences pending", _pendingRevMessages,
                       _missingSequences.size());
        }
    }

    // We lost access to some documents; they need to be purged locally.
    void Puller::_documentsRevoked(std::vector<Retained<RevToInsert>> revs) {
        for ( auto& rev : revs ) {
            if ( _activeIncomingRevoked < tuning::kMaxActiveIncomingRevs
                 && _unfinishedIncomingRevoked < tuning::kMaxIncomingRevs ) {
                startRevoked(rev);
            } else {
                logDebug("Delaying handling revocation for '%.*s' [%zu waiting]", SPLAT(rev->docID),
                         _waitingRevoked.size() + 1);
                if ( _waitingRevoked.empty() ) { logVerbose("Back pressure started for revocations"); }
                _waitingRevoked.push_back(std::move(rev));
            }
        }
    }

    // Received an incoming "rev" message, which contains a revision body to insert
    void Puller::handleRev(Retained<MessageIn> msg) {
        if ( _activeIncomingRevs < tuning::kMaxActiveIncomingRevs
             && _unfinishedIncomingRevs < tuning::kMaxIncomingRevs ) {
            startIncomingRev(msg);
        } else {
            logDebug("Delaying handling 'rev' message for '%.*s' [%zu waiting]", SPLAT(msg->property("id"_sl)),
                     _waitingRevMessages.size() + 1);
            if ( _waitingRevMessages.empty() ) {
                Signpost::begin(Signpost::revsBackPressure);
                logVerbose("Back pressure started for changes messages");
            }
            _waitingRevMessages.push_back(std::move(msg));
        }
    }

    // Received an incoming "norev" message, which means the peer was unable to send a revision
    void Puller::handleNoRev(Retained<MessageIn> msg) {
        _revFinder->revReceived();
        decrement(_pendingRevMessages);
        slice sequence(msg->property("sequence"_sl));
        if ( sequence ) completedSequence(RemoteSequence(sequence));
        if ( !msg->noReply() ) {
            MessageBuilder response(msg);
            msg->respond(response);
        }
    }

    // Actually process an incoming "rev" now:
    void Puller::startIncomingRev(MessageIn* msg) {
        _revFinder->revReceived();
        decrement(_pendingRevMessages);
        Retained<IncomingRev> inc = makeIncomingRev<false>();
        if ( inc ) {
            slice sequenceStr = msg->property(slice("sequence"));
            inc->handleRev(msg, _missingSequences.bodySizeOfSequence(RemoteSequence(
                                        sequenceStr)));  // ... will call _revWasHandled when it's finished
        }
    }

    // Actually process a revocation now
    void Puller::startRevoked(RevToInsert* rev) {
        Retained<IncomingRev> inc = makeIncomingRev<true>();
        if ( inc ) inc->handleRevokedDoc(rev);
    }

    // Sets up an IncomingRev object to handle a revision.
    template <bool revoked>
    Retained<IncomingRev> Puller::makeIncomingRev() {
        if ( !connected() ) {
            // Connection already closed, continuing would cause a crash
            logVerbose("makeIncomingRev called after connection close, ignoring...");
            return nullptr;
        }
        if constexpr ( revoked ) {
            increment(_activeIncomingRevoked);
            increment(_unfinishedIncomingRevoked);
        } else {
            increment(_activeIncomingRevs);
            increment(_unfinishedIncomingRevs);
        }

        Retained<IncomingRev> inc;
        if ( _spareIncomingRevs.empty() ) {
            inc = new IncomingRev(this);
            inc->_log(LogLevel::Info, "CBL-7347 new IncomingRev inc=%p", inc.get());
        } else {
            inc = _spareIncomingRevs.back();
            _spareIncomingRevs.pop_back();
            inc->_log(LogLevel::Info, "CBL-7347 spare IncomingRev inc=%p", inc.get());
        }
        return inc;
    }

    void Puller::maybeStartIncomingRevs() {
        while ( connected() && _activeIncomingRevs < tuning::kMaxActiveIncomingRevs
                && _unfinishedIncomingRevs < tuning::kMaxIncomingRevs && !_waitingRevMessages.empty() ) {
            auto msg = _waitingRevMessages.front();
            _waitingRevMessages.pop_front();
            if ( _waitingRevMessages.empty() ) {
                Signpost::end(Signpost::revsBackPressure);
                logVerbose("Back pressure ended for changes messages");
            }
            startIncomingRev(msg);
        }
        while ( !_waitingRevoked.empty() && _activeIncomingRevoked < tuning::kMaxActiveIncomingRevs
                && _unfinishedIncomingRevoked < tuning::kMaxIncomingRevs ) {
            auto rev = _waitingRevoked.front();
            _waitingRevoked.pop_front();
            if ( _waitingRevMessages.empty() ) { logVerbose("Back pressure ended for revocations"); }
            startRevoked(rev);
        }
    }

    // Callback from an IncomingRev when it's been written to the db, but before the commit
    void Puller::_revsWereProvisionallyHandled() {
        auto count = _provisionallyHandledRevs.take();
        if ( count > 0 ) {
            decrement(_activeIncomingRevs, count);
            _logVerbose("%u revs were provisionally handled; down to %u active", count, _activeIncomingRevs);
        }
        count = _provisionallyHandledRevoked.take();
        if ( count > 0 ) {
            decrement(_activeIncomingRevoked, count);
            _logVerbose("%u revocations were provisionally handled; down to %u active", count, _activeIncomingRevoked);
        }
        maybeStartIncomingRevs();
    }

    // Called from an IncomingRev when it's finished (either added to db, or failed.)
    // The IncomingRev will be processed later by _revsFinished().
    void Puller::revWasHandled(IncomingRev* inc) {
        // CAUTION: For performance reasons this method is called directly, without going through the
        // Actor event queue, so it runs on the IncomingRev's thread, NOT the Puller's! Thus, it needs
        // to pay attention to thread-safety.
        _returningRevs.push(inc);  // this is thread-safe
    }

    void Puller::_revsFinished(int gen) {
        auto revs = _returningRevs.pop(gen);
        if ( !revs ) { return; }
        unsigned numRevoked = 0;
        for ( IncomingRev* inc : *revs ) {
            // If it was provisionally inserted, _activeIncomingRevs will have been decremented
            // already (in _revsWereProvisionallyHandled.) If not, decrement now:
            inc->_log(LogLevel::Info, "CBL-7347 inc=%p in _revsFinished", inc);
            auto rev = inc->rev();
            inc->_log(LogLevel::Info, "CBL-7347 rev=%p in _revsFinished", rev);

            if ( rev->revocationMode == RevocationMode::kNone ) {
                if ( !inc->wasProvisionallyInserted() ) decrement(_activeIncomingRevs);
                decrement(_unfinishedIncomingRevs);
            } else {
                if ( !inc->wasProvisionallyInserted() ) decrement(_activeIncomingRevoked);
                numRevoked++;
            }
            if ( !passive() ) completedSequence(inc->remoteSequence(), rev->errorIsTransient, false);
            finishedDocument(rev);
            inc->reset();
        }

        if ( numRevoked > 0 ) {
            decrement(_unfinishedIncomingRevoked, numRevoked);
            _revFinder->revokedHandled(numRevoked);
        }

        ssize_t capacity = tuning::kMaxIncomingRevs - narrow_cast<ssize_t>(_spareIncomingRevs.size());
        if ( capacity > 0 )
            _spareIncomingRevs.insert(_spareIncomingRevs.end(), revs->begin(),
                                      revs->begin() + min(capacity, narrow_cast<ssize_t>(revs->size())));

        if ( !passive() ) updateLastSequence();

        maybeStartIncomingRevs();
    }

    void Puller::revReRequested(uint64_t missingBodySize) {
        enqueue(FUNCTION_TO_QUEUE(Puller::_revReRequested), missingBodySize);
    }

    void Puller::_revReRequested(uint64_t missingBodySize) {
        // Regression from CBL-936 / CBG-881:  Because after a delta failure the full revision is
        // requested without another changes message, this needs to be bumped back up because it
        // won't get another changes message to bump it.
        increment(_pendingRevMessages);
        _revFinder->reRequestingRev();
        addProgress({0, missingBodySize});
    }

    // Records that a sequence has been successfully pulled.
    void Puller::completedSequence(const RemoteSequence& sequence, bool withTransientError,
                                   bool shouldUpdateLastSequence) {
        uint64_t bodySize;
        if ( withTransientError ) {
            // If there's a transient error, don't mark this sequence as completed,
            // but add the body size to the completed so the progress will reach 1.0
            bodySize = _missingSequences.bodySizeOfSequence(sequence);
        } else {
            bool wasEarliest;
            _missingSequences.remove(sequence, wasEarliest, bodySize);
            if ( wasEarliest && shouldUpdateLastSequence ) updateLastSequence();
        }
        addProgress({bodySize, 0});
    }

    void Puller::updateLastSequence() {
        auto since = _missingSequences.since();
        if ( since != _lastSequence ) {
            _lastSequence = since;
            logVerbose("Checkpoint now at '%s'", _lastSequence.toJSONString().c_str());
            if ( auto replicator = replicatorIfAny(); replicator )
                replicator->checkpointer(collectionIndex()).setRemoteMinSequence(_lastSequence);
        }
    }

    void Puller::insertRevision(RevToInsert* rev) { _inserter->insertRevision(rev); }

#pragma mark - STATUS / PROGRESS:

    void Puller::_childChangedStatus(Retained<Worker> task, Status status) {
        // Combine the IncomingRev's progress into mine:
        addProgress(status.progressDelta);
        if ( status.error.domain == WebSocketDomain && status.error.code == 503 ) {
            if ( _parent ) _parent->childChangedStatus(this, status);
        }
    }

    namespace {
        enum ReasonCode : uint8_t {
            rcUnfinishedIncomingRevs,
            rcUnfinishedIncomingRevoked,
            rcFatalError,
            rcNotConnected,
            rcParentLevel,
            rcNotCaughtUp,
            rcPendingRevMessages,
            rcContinuous,
            rcOpenServer,
            rcOneShotFinished,

            rcEnd
        };

        const char* const reasonTable[rcEnd]{"unfinishedIncomingRevs",
                                             "unfinishedIncomingRevoked",
                                             "fatalError",
                                             "notConnected",
                                             nullptr,  // dynamically available in parentReason
                                             "notCaughtUp",
                                             "pendingRevMessages",
                                             "continuous",
                                             "openServer",
                                             "oneShotFinished"};
    }  // namespace

    Worker::ActivityLevel Puller::computeActivityLevel(std::string* reason) const {
        ActivityLevel                                level;
        ReasonCode                                   rc{rcEnd};
        std::string                                  parentReason;
        Worker::ActivityLevel                        parentLevel{kC4Stopped};
        std::vector<std::pair<ReasonCode, unsigned>> counters;
        counters.reserve(2);
        if ( _unfinishedIncomingRevs + _unfinishedIncomingRevoked > 0 ) {
            // CBL-221: Crash when scheduling document ended events
            level = kC4Busy;
            if ( _unfinishedIncomingRevs > 0 ) {
                rc = rcUnfinishedIncomingRevs;
                counters.emplace_back(rc, _unfinishedIncomingRevs);
            } else {
                rc = rcUnfinishedIncomingRevoked;
                counters.emplace_back(rc, _unfinishedIncomingRevoked);
            }
        } else if ( _fatalError || !connected() ) {
            level = kC4Stopped;
            rc    = _fatalError ? rcFatalError : rcNotConnected;
        } else if ( (parentLevel = Worker::computeActivityLevel(reason ? &parentReason : nullptr)) == kC4Busy
                    || (!_caughtUp && !passive()) || _pendingRevMessages > 0 ) {
            level = kC4Busy;
            rc    = parentLevel == kC4Busy       ? rcParentLevel
                    : (!_caughtUp && !passive()) ? rcNotCaughtUp
                                                 : rcPendingRevMessages;
            if ( rc == rcPendingRevMessages ) counters.emplace_back(rc, _pendingRevMessages);
        } else if ( _options->pull(collectionIndex()) == kC4Continuous || isOpenServer() ) {
            _spareIncomingRevs.clear();
            level = kC4Idle;
            rc    = _options->pull(collectionIndex()) == kC4Continuous ? rcContinuous : rcOpenServer;
        } else {
            level = kC4Stopped;
            rc    = rcOneShotFinished;
        }

        if ( reason ) {
            *reason = reasonTable[rc] ? reasonTable[rc] : parentReason;
            for ( const auto& counter : counters ) {
                if ( counter.first == rc ) {
                    *reason = stringprintf("%s/%u", reason->c_str(), counter.second);
                    break;
                }
            }
        }

        if ( SyncBusyLog.willLog(LogLevel::Info) ) {
            logInfo("activityLevel=%-s: pendingResponseCount=%d, _caughtUp=%d,"
                    " _pendingRevMessages=%u, _activeIncomingRevs=%u, _waitingRevMessages=%zu,"
                    " _unfinishedIncomingRevs=%u",
                    kC4ReplicatorActivityLevelNames[level], pendingResponseCount(), _caughtUp, _pendingRevMessages,
                    _activeIncomingRevs, _waitingRevMessages.size(), _unfinishedIncomingRevs);
        }

        if ( level == kC4Stopped ) _revFinder = nullptr;  // break cycle

        return level;
    }
}  // namespace litecore::repl
