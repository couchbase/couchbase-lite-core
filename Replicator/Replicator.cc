//
// Replicator.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//
//  https://github.com/couchbase/couchbase-lite-core/wiki/Replication-Protocol

#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "Pusher.hh"
#include "Puller.hh"
#include "Checkpoint.hh"
#include "DBAccess.hh"
#include "Delimiter.hh"
#include "c4Database.hh"
#include "c4DocEnumerator.hh"
#include "c4SocketTypes.h"           // for error codes
#include "Error.hh"
#include "StringUtil.hh"
#include "Logging.hh"
#include "Headers.hh"
#include "BLIP.hh"
#include "Address.hh"
#include "Instrumentation.hh"
#include "fleece/Mutable.hh"

using namespace std;
using namespace std::placeholders;
using namespace fleece;
using namespace litecore::blip;


namespace litecore { namespace repl {

    struct StoppingErrorEntry {
        C4Error err;
        bool isFatal;
        slice msg;
    };

    // Errors treated specially by onError()
    static constexpr StoppingErrorEntry StoppingErrors[] = {
        {{ LiteCoreDomain, kC4ErrorUnexpectedError,0 }, true, "An exception was thrown"_sl},
        {{ WebSocketDomain, 403, 0}, true, "An attempt was made to perform an unauthorized action"_sl},
        {{ WebSocketDomain, 503, 0 }, false, "The server is over capacity"_sl}
    };

                             
    std::string Replicator::ProtocolName() {
        stringstream result;
        delimiter delim(",");
        for (auto &name : kCompatProtocols)
            result << delim << name;
        return result.str();
    }


    Replicator::Replicator(C4Database* db,
                           websocket::WebSocket *webSocket,
                           Delegate &delegate,
                           Options *options)
    :Worker(new Connection(webSocket, options->properties, *this),
            nullptr,
            options,
            make_shared<DBAccess>(db, options->properties["disable_blob_support"_sl].asBool()),
            "Repl",
            kNotCollectionIndex)
    ,_delegate(&delegate)
    ,_pushers(options->collectionCount())
    ,_pullers(options->collectionCount())
    ,_connectionState(connection().state())
    ,_pushStatusV(_options->collectionCount())
    ,_pullStatusV(_options->collectionCount())
    ,_docsEnded(this, "docsEnded", &Replicator::notifyEndedDocuments, tuning::kMinDocEndedInterval, 100)
    ,_checkpointer(_options->collectionCount())
    ,_hadLocalCheckpoint(_options->collectionCount())
    ,_remoteCheckpointRequested(_options->collectionCount())
    ,_remoteCheckpointReceived(_options->collectionCount())
    ,_checkpointJSONToSave(_options->collectionCount())
    ,_remoteCheckpointDocID(_options->collectionCount())
    ,_remoteCheckpointRevID(_options->collectionCount())
    {
        _loggingID = string(db->getPath()) + " " + _loggingID;
        _importance = 2;

        logInfo("%s", string(*options).c_str());
        Assert(_options->collectionCount() > 0 && _options->collectionCount() < kNotCollectionIndex);
        Assert(_options->hasPassiveCollection ? _options->isAllPassive : true);

        // Retained C4Collection object may become invalid if the underlying collection
        // is deleted. By spec, all collections must exist when replication starts,
        // and it is an error if any collection is deleted while in progress.
        // Note: retained C4Collection* may blow up if it is used after becoming invalid,
        // and this is expected.
        _collections.reserve(_options->collectionCount());
        _db->useLocked([this](Retained<C4Database>& db) {
            _options->forEachCollection([this,&db](unsigned, const Options::CollectionOptions& coll) {
                C4Collection* c = db->getCollection(Options::collectionPathToSpec(coll.collectionPath));
                if (c == nullptr) {
                    error::_throw(error::UnexpectedError);
                }
                this->_collections.push_back(c);
            });
        });

        actor::Timer::duration saveDelay = tuning::kDefaultCheckpointSaveDelay;
        if (auto i = options->properties[kC4ReplicatorCheckpointInterval].asInt(); i > 0)
            saveDelay = chrono::seconds(i);

        bool isPushBusy = false;
        bool isPullBusy = false;
        for (CollectionIndex i = 0; i < _options->collectionCount(); ++i) {
            if (_options->pushOf(i) != kC4Disabled) {
                _checkpointer[i].reset(new Checkpointer(_options, webSocket->url(), _collections[i]));
                _pushers[i] = new Pusher(this, *_checkpointer[i], i);
                _pushStatusV[i] = kC4Busy;
                isPushBusy = true;
            } else {
                _pushStatusV[i] = kC4Stopped;
            }
            if (_options->pullOf(i) != kC4Disabled) {
                _pullers[i] = new Puller(this, i);
                _pullStatusV[i] = kC4Busy;
                if (_checkpointer[i].get() == nullptr) {
                    _checkpointer[i].reset(new Checkpointer(_options, webSocket->url(), _collections[i]));
                }
                isPullBusy = true;
            } else {
                _pullStatusV[i] = kC4Stopped;
            }
            if (_checkpointer[i].get() != nullptr) {
                _checkpointer[i]->enableAutosave(saveDelay, bind(&Replicator::saveCheckpoint, this, i, _1));
            }
        }
        _pushStatus = isPushBusy ? kC4Busy : kC4Stopped;
        _pullStatus = isPullBusy ? kC4Busy : kC4Stopped;

        if (std::none_of(_pushers.begin(), _pushers.end(), [](const Retained<Pusher>& p) {
            return p;
        })) {
            for (auto profile : {"subChanges", "getAttachment", "proveAttachment"})
                registerHandler(profile,  &Replicator::returnForbidden);
        }
        if (std::none_of(_pullers.begin(), _pullers.end(), [](const Retained<Puller>& p) {
            return p;
        })) {
            for (auto profile : {"changes", "proposeChanges", "rev", "norev"})
                registerHandler(profile,  &Replicator::returnForbidden);
        }

        registerHandler("getCheckpoint",    &Replicator::handleGetCheckpoint);
        registerHandler("setCheckpoint",    &Replicator::handleSetCheckpoint);
        registerHandler("getCollections",   &Replicator::handleGetCollections);
    }


    void Replicator::start(bool reset, bool synchronous) {
        if (synchronous)
            _start(reset);
        else
            enqueue(FUNCTION_TO_QUEUE(Replicator::_start), reset);
    }


    void Replicator::_start(bool reset) {
        try {
            Assert(_connectionState == Connection::kClosed);
            Signpost::begin(Signpost::replication, uintptr_t(this));
            _connectionState = Connection::kConnecting;
            connection().start();
            // Now wait for _onConnect or _onClose...

            _findExistingConflicts();

            // Get the remote DB ID:
            slice key;
            // Assertion: _collections.size() > 0
            // All _checkpointer's share the same key.
            key = _checkpointer[0]->remoteDBIDString();
            C4RemoteID remoteDBID = _db->lookUpRemoteDBID(key);
            logVerbose("Remote-DB ID %u found for target <%.*s>", remoteDBID, SPLAT(key));

            for (CollectionIndex i = 0; i < _collections.size(); ++i) {
                if (_options->pushOf(i) > kC4Passive || _options->pullOf(i) > kC4Passive) {
                    // Get the checkpoints:
                    if (getLocalCheckpoint(reset, i)) {
                        getRemoteCheckpoint(false, i);
                    }
                }
            }
        } catch (...) {
            C4Error err = C4Error::fromCurrentException();
            logError("Failed to start replicator: %s", err.description().c_str());
            gotError(err);
            stop();
        }
    }


    void Replicator::_findExistingConflicts() {
        Stopwatch st;
        for (CollectionIndex i = 0; i < _collections.size(); ++i) {
            if (_options->pullOf(i) <= kC4Passive) // only check in pull mode
                continue;

            try {
                unique_ptr<C4DocEnumerator> e = _db->unresolvedDocsEnumerator(false, _collections[i]);
                logInfo("Scanning for pre-existing conflicts (collection: %u)...", i);
                unsigned nConflicts = 0;
                while (e->next()) {
                    C4DocumentInfo info = e->documentInfo();
                    auto rev = retained(new RevToInsert(nullptr,        /* incoming rev */
                                                        info.docID,
                                                        info.revID,
                                                        nullslice,      /* history buf */
                                                        info.flags & kDocDeleted,
                                                        false,
                                                        _collections[i]->getSpec()));
                    rev->error = C4Error::make(LiteCoreDomain, kC4ErrorConflict);
                    _docsEnded.push(rev);
                    ++nConflicts;
                }
                logInfo("Found %u conflicted docs in %.3f sec (collection: %u)",
                        nConflicts, st.elapsed(), i);
            } catch (...) {
                C4Error err = C4Error::fromCurrentException();
                warn("Couldn't get unresolved docs enumerator: error %d/%d", err.domain, err.code);
                gotError(err);
            }
        }
    }


    void Replicator::_stop() {
        logInfo("Told to stop!");
        _disconnect(websocket::kCodeNormal, {});
    }


    void Replicator::terminate() {
        logDebug("terminate() called...");
        if (connected()) {
            logDebug("...connected() was true, doing extra stuff...");
            Assert(_connectionState == Connection::kClosed);
            connection().terminate();
            std::for_each(_pushers.begin(), _pushers.end(), [](Retained<Pusher>& p) {
                p = nullptr;
            });
            std::for_each(_pullers.begin(), _pullers.end(), [](Retained<Puller>& p) {
                p = nullptr;
            });
        }

        // CBL-1061: This used to be inside the connected(), but static analysis shows
        // terminate() is only called from the destructor of the _delegate itself, so it is
        // dangerous to leave it around.  Set it to null here to avoid using it further.
        _delegate = nullptr;
        _db.reset();
        logDebug("...done with terminate()");
    }


    void Replicator::_disconnect(websocket::CloseCode closeCode, slice message) {
        if (connected()) {
            connection().close(closeCode, message);
            _connectionState = Connection::kClosing;
        }
    }


    // Called after the checkpoint is established.
    void Replicator::startReplicating(CollectionIndex coll) {
        if (_options->pushOf(coll) > kC4Passive)
             _pushers[coll]->start();
         if (_options->pullOf(coll) > kC4Passive)
             _pullers[coll]->start(_checkpointer[coll]->remoteMinSequence());
    }


    void Replicator::docRemoteAncestorChanged(alloc_slice docID, alloc_slice revID,
                                              CollectionIndex coll) {
        Retained<Pusher> pusher = _pushers[coll];
        if (pusher)
            pusher->docRemoteAncestorChanged(docID, revID);
    }


    void Replicator::returnForbidden(Retained<blip::MessageIn> request) {
        auto collectionIn = request->intProperty("collection"_sl, kNotCollectionIndex);
        CollectionIndex c = 0;
        if (collectionIn != kNotCollectionIndex) {
            c = (CollectionIndex)collectionIn;
        } else {
            warn("\"collection\" property is not present in the request; 0 is used");
        }
        if (_options->pushOf(c) != kC4Disabled) {
            request->respondWithError(Error("HTTP"_sl, 403, "Attempting to push to a pull-only replicator"_sl));
        } else {
            request->respondWithError(Error("HTTP"_sl, 403, "Attempting to pull from a push-only replicator"_sl));
        }
    }


#pragma mark - STATUS:

    string Replicator::statusVString() const {
        std::stringstream ss;
        for (CollectionIndex i = 0; i < _pushStatusV.size(); ++i) {
            if (i > 0) {
                ss << '|';
            }
            ss << "pushStatus=" << kC4ReplicatorActivityLevelNames[_pushStatusV[i].level];
            ss << ", pullStatus=" << kC4ReplicatorActivityLevelNames[_pullStatusV[i].level];
            ss << ", progress=" << _pushStatusV[i].progress.unitsCompleted
                                 + _pullStatusV[i].progress.unitsCompleted;
            ss << "/";
            ss << _pushStatusV[i].progress.unitsTotal
                + _pullStatusV[i].progress.unitsTotal;
        }
        return ss.str();
    }

    void Replicator::updatePushStatus(CollectionIndex i, const Status& status) {
        // Status::level
        if (status.level >= _pushStatus.level) {
            _pushStatusV[i].level = status.level;
            _pushStatus.level = status.level;
        } else {
            auto prevLevel = _pushStatusV[i].level;
            _pushStatusV[i].level = status.level;
            if (prevLevel >= _pushStatus.level) {
                auto it = std::max_element(_pushStatusV.begin(), _pushStatusV.end(),
                                           [](const Status& a, const Status&b) {
                                              return a.level < b.level;
                                           });
                _pushStatus.level = it->level;
            }
        }

        // Status::progress
        auto delta = status.progress - _pushStatusV[i].progress;
        _pushStatusV[i].progress = status.progress;
        _pushStatus.progress += delta;

        // Status::error
        _pushStatusV[i].error = status.error;
        if (_pushStatus.error.code == 0) {
            // overall error moves from 0 to non-0.
            // TBD: how to not lose still more severe error?
            _pushStatus.error = status.error;
        }
    }

    void Replicator::updatePullStatus(CollectionIndex i, const Status& status) {
        // Status::level
        if (status.level >= _pullStatus.level) {
            _pullStatusV[i].level = status.level;
            _pullStatus.level = status.level;
        } else {
            auto prevLevel = _pullStatusV[i].level;
            _pullStatusV[i].level = status.level;
            if (prevLevel >= _pullStatus.level) {
                auto it = std::max_element(_pullStatusV.begin(), _pullStatusV.end(),
                                           [](const Status& a, const Status&b) {
                                              return a.level < b.level;
                                           });
                _pullStatus.level = it->level;
            }
        }
        _pullStatusV[i].level = status.level;
        if (status.level > _pullStatus.level) {
            _pullStatus.level = status.level;
        }

        // Status::progress
        auto delta = status.progress - _pullStatusV[i].progress;
        _pullStatusV[i].progress = status.progress;
        _pullStatus.progress += delta;

        // Status::error
        _pullStatusV[i].error = status.error;
        if (_pullStatus.error.code == 0) {
            // overall error moves from 0 to non-0.
            // TBD: how to not lose still more severe error?
            _pullStatus.error = status.error;
        }
    }

    // The status of one of the actors has changed; update mine
    void Replicator::_childChangedStatus(Retained<Worker> task, Status taskStatus) {
        if (status().level == kC4Stopped)       // I've already stopped & cleared refs; ignore this
            return;

        CollectionIndex coll = task->collectionIndex();
        if (coll != kNotCollectionIndex) {
            if (task == _pushers[coll]) {
                updatePushStatus(coll, taskStatus);
            } else if (task == _pullers[coll]) {
                updatePullStatus(coll, taskStatus);
            }
        }

        setProgress(_pushStatus.progress + _pullStatus.progress);

        if (SyncBusyLog.willLog(LogLevel::Info)) {
            logInfo("pushStatus=%-s, pullStatus=%-s, progress=%" PRIu64 "/%" PRIu64 "",
                    kC4ReplicatorActivityLevelNames[_pushStatus.level],
                    kC4ReplicatorActivityLevelNames[_pullStatus.level],
                    status().progress.unitsCompleted, status().progress.unitsTotal);
        }
        if (SyncBusyLog.willLog(LogLevel::Verbose)) {
            logVerbose("Replicator status collection-wise: %s", statusVString().c_str());
        }

        for (CollectionIndex i = 0; i < _collections.size(); ++i) {
            if (_pullStatusV[i].error.code)
                onError(_pullStatusV[i].error);
            else if (_pushStatusV[i].error.code)
                onError(_pushStatusV[i].error);
        }

        if (coll != kNotCollectionIndex) {
            // Save a checkpoint immediately when push or pull finishes or goes idle:
            if (taskStatus.level == kC4Stopped || taskStatus.level == kC4Idle)
                _checkpointer[coll]->save();
        }
    }


    Worker::ActivityLevel Replicator::computeActivityLevel() const {
        // Once I've announced I've stopped, don't return any other status again:
        auto currentLevel = status().level;
        if (currentLevel == kC4Stopped)
            return kC4Stopped;

        ActivityLevel level = kC4Busy;
        bool hasUnsaved = false;
        switch (_connectionState) {
            case Connection::kConnecting:
                level = kC4Connecting;
                break;
            case Connection::kConnected: {
                hasUnsaved = std::any_of(_checkpointer.begin(), _checkpointer.end(),
                                         [](const unique_ptr<Checkpointer>& chkpointer) {
                    return chkpointer->isUnsaved();
                });
                if (hasUnsaved)
                    level = kC4Busy;
                else
                    level = Worker::computeActivityLevel();
                level = max(level, max(_pushStatus.level, _pullStatus.level));
                if (level == kC4Idle && !isContinuous() && !isOpenServer()) {
                    // Detect that a non-continuous active push or pull replication is done:
                    logInfo("Replication complete! Closing connection");
                    const_cast<Replicator*>(this)->_stop();
                    level = kC4Busy;
                }
                DebugAssert(level > kC4Stopped);
                break;
            }
            case Connection::kClosing:
                // Remain active while I wait for the connection to finish closing:
                logDebug("Connection closing... (activityLevel=busy)waiting to finish");
                level = kC4Busy;
                break;
            case Connection::kDisconnected:
            case Connection::kClosed:
                // After connection closes, remain Busy (or Connecting) while I wait for db to
                // finish writes and for myself to process any pending messages; then go to Stopped.
                level = Worker::computeActivityLevel();
                level = max(level, max(_pushStatus.level, _pullStatus.level));
                if (level < kC4Busy)
                    level = kC4Stopped;
                else if (currentLevel == kC4Connecting)
                    level = kC4Connecting;
                break;
        }
        if (SyncBusyLog.willLog(LogLevel::Info)) {
            logInfo("activityLevel=%-s: connectionState=%d, savingChkpt=%d",
                    kC4ReplicatorActivityLevelNames[level], _connectionState, hasUnsaved);
        }
        return level;
    }


    void Replicator::onError(C4Error error) {
        if(status().error.code != 0 && error.domain == WebSocketDomain &&
           (error.code == kWebSocketCloseAppPermanent || error.code == kWebSocketCloseAppTransient)) {
            // CBL-1178: If we already have an error code, it is more relevant than the web socket close code, so keep it
            // intact so that the consumer can know what went wrong
            logVerbose("kWebSocketCloseAppPermanent or kWebSocketCloseAppTransient received, ignoring (only relevant for underlying connection...)");
            return;
        }
        
        Worker::onError(error);
        for (const StoppingErrorEntry& stoppingErr : StoppingErrors) {
            if (stoppingErr.err == error) {
                string message = error.description().c_str();
                if (stoppingErr.isFatal) {
                    logError("Stopping due to fatal error: %s", message.c_str());
                    _disconnect(websocket::kCloseAppPermanent, stoppingErr.msg);
                } else {
                    logError("Stopping due to error: %s", message.c_str());
                    _disconnect(websocket::kCloseAppTransient, stoppingErr.msg);
                }
                return;
            }
        }
    }


    void Replicator::changedStatus() {
        if (status().level == kC4Stopped) {
            DebugAssert(!connected());  // must already have gotten _onClose() delegate callback
            _pushers.clear();
            _pullers.clear();
            _db->close();
            Signpost::end(Signpost::replication, uintptr_t(this));
        }
        if (_delegate) {
            // Notify the delegate of the current status, but not too often:
            auto waitFor = tuning::kMinDelegateCallInterval - _sinceDelegateCall.elapsedDuration();
            if (waitFor <= 0s || status().level != _lastDelegateCallLevel) {
                reportStatus();
            } else if (!_waitingToCallDelegate) {
                _waitingToCallDelegate = true;
                enqueueAfter(waitFor, FUNCTION_TO_QUEUE(Replicator::reportStatus));
            }
        }
    }


    void Replicator::reportStatus() {
        _waitingToCallDelegate = false;
        _lastDelegateCallLevel = status().level;
        _sinceDelegateCall.reset();
        if (_delegate) {
            notifyEndedDocuments();
            _delegate->replicatorStatusChanged(this, status());
        }
        if (status().level == kC4Stopped)
            _delegate = nullptr;        // Never call delegate after telling it I've stopped
    }


    void Replicator::endedDocument(ReplicatedRev *d) {
        logInfo("documentEnded %.*s %.*s flags=%02x (%d/%d)", SPLAT(d->docID), SPLAT(d->revID),
                 d->flags, d->error.domain, d->error.code);
        d->trim(); // free up unneeded stuff
        if (_delegate) {
            if (d->isWarning && (d->flags & kRevIsConflict)) {
                // Inserter::insertRevisionNow set this flag to indicate that the rev caused a
                // conflict (though it did get inserted), so notify the delegate of the conflict:
                d->error = C4Error::make(LiteCoreDomain, kC4ErrorConflict, nullslice);
                d->errorIsTransient = true;
            }
            _docsEnded.push(d);
        }
    }


    void Replicator::notifyEndedDocuments(int gen) {
        auto docs = _docsEnded.pop(gen);
        if (docs && !docs->empty() && _delegate)
            _delegate->replicatorDocumentsEnded(this, *docs);
    }


    void Replicator::_onBlobProgress(BlobProgress p) {
        if (_delegate)
            _delegate->replicatorBlobProgress(this, p);
    }


#pragma mark - BLIP DELEGATE:


    void Replicator::onTLSCertificate(slice certData) {
        if (_delegate)
            _delegate->replicatorGotTLSCertificate(certData);
    }


    void Replicator::onHTTPResponse(int status, const websocket::Headers &headers) {
        enqueue(FUNCTION_TO_QUEUE(Replicator::_onHTTPResponse), status, headers);
    }


    void Replicator::_onHTTPResponse(int status, websocket::Headers headers) {
        if (status == 101 && !headers["Sec-WebSocket-Protocol"_sl]) {
            gotError(C4Error::make(WebSocketDomain, kWebSocketCloseProtocolError,
                                  "Incompatible replication protocol "
                                  "(missing 'Sec-WebSocket-Protocol' response header)"_sl));
        }
        if (_delegate)
            _delegate->replicatorGotHTTPResponse(this, status, headers);
    }


    void Replicator::_onConnect() {
        logInfo("Connected!");
        Signpost::mark(Signpost::replicatorConnect, uintptr_t(this));
        if (_connectionState != Connection::kClosing) {     // skip this if stop() already called
            _connectionState = Connection::kConnected;
            for (CollectionIndex i = 0; i < _collections.size(); ++i) {
                if (_options->pushOf(i) > kC4Passive || _options->pullOf(i) > kC4Passive)
                    getRemoteCheckpoint(false, i);
            }
        }
    }


    void Replicator::_onClose(Connection::CloseStatus status, Connection::State state) {
        logInfo("Connection closed with %-s %d: \"%.*s\" (state=%d)",
            status.reasonName(), status.code, SPLAT(status.message), _connectionState);
        Signpost::mark(Signpost::replicatorDisconnect, uintptr_t(this));

        bool closedByPeer = (_connectionState != Connection::kClosing);
        _connectionState = state;

        std::for_each(_checkpointer.begin(), _checkpointer.end(),
                      [](unique_ptr<Checkpointer>& chkpointer) {
            chkpointer->stopAutosave();
        });

        // Clear connection() and notify the other agents to do the same:
        _connectionClosed();
        for (CollectionIndex i = 0; i < _pushers.size(); ++i) {
            if (_pushers[i])
                _pushers[i]->connectionClosed();
            if (_pullers[i])
                _pullers[i]->connectionClosed();
        }

        // TBD: Check with Jens
        bool anyActive = false;
        for (CollectionIndex i = 0; i < _collections.size(); ++i) {
            if (_options->pushOf(i) > kC4Passive || _options->pullOf(i) > kC4Passive) {
                anyActive = true;
                break;
            }
        }
        if (status.isNormal() && closedByPeer && anyActive) {
            logInfo("I didn't initiate the close; treating this as code 1001 (GoingAway)");
            status.code = websocket::kCodeGoingAway;
            status.message = alloc_slice("WebSocket connection closed by peer");
        }

        static const C4ErrorDomain kDomainForReason[] = {WebSocketDomain, POSIXDomain,
                                                         NetworkDomain, LiteCoreDomain};

        // If this was an unclean close, set my error property:
        if (status.reason != websocket::kWebSocketClose || status.code != websocket::kCodeNormal) {
            int code = status.code;
            C4ErrorDomain domain;
            if (status.reason < sizeof(kDomainForReason)/sizeof(C4ErrorDomain)) {
                domain = kDomainForReason[status.reason];
            } else {
                domain = LiteCoreDomain;
                code = kC4ErrorRemoteError;
            }
            gotError(C4Error::make(domain, code, status.message));
        }

        if (_delegate) {
            notifyEndedDocuments();
            _delegate->replicatorConnectionClosed(this, status);
        }
    }


    // This only gets called if none of the registered handlers were triggered.
    void Replicator::_onRequestReceived(Retained<MessageIn> msg) {
        CollectionIndex collection = (CollectionIndex)msg->intProperty("collection"_sl, kNotCollectionIndex);
        warn("Received unrecognized BLIP request #%" PRIu64 "(collection: %u) with Profile '%.*s', %zu bytes",
                msg->number(), collection, SPLAT(msg->property("Profile"_sl)), msg->body().size);
        msg->notHandled();
    }


#pragma mark - CHECKPOINT:


    // Start off by getting the local checkpoint, if this is an active replicator:
    bool Replicator::getLocalCheckpoint(bool reset, CollectionIndex coll) {
        try {
            if (_checkpointer[coll]->read(_db->useLocked(), reset)) {
                auto remote = _checkpointer[coll]->remoteMinSequence();
                logInfo("Read local checkpoint '%.*s': %.*s (collection: %u)",
                        SPLAT(_checkpointer[coll]->initialCheckpointID()),
                        SPLAT(_checkpointer[coll]->checkpointJSON()),
                        coll);
                _hadLocalCheckpoint[coll] = true;
            } else if (reset) {
                logInfo("Ignoring local checkpoint ('reset' option is set) (collection: %u)", coll);
            } else {
                logInfo("No local checkpoint '%.*s' (collection: %u)", SPLAT(_checkpointer[coll]->initialCheckpointID()), coll);
                // If pulling into an empty db with no checkpoint, it's safe to skip deleted
                // revisions as an optimization.
                C4Collection* collection = _collections[coll];
                if (_options->pullOf(coll) > kC4Passive && _pullers[coll]
                        && _db->useCollection(collection)->getLastSequence() == 0_seq)
                    _pullers[coll]->setSkipDeleted();
            }
            return true;
        } catch (...) {
            logInfo("Fatal error getting local checkpoint (collection: %u)", coll);
            gotError(C4Error::fromCurrentException());
            stop();
            return false;
        }
    }


    // Get the remote checkpoint, after we've got the local one and the BLIP connection is up.
    void Replicator::getRemoteCheckpoint(bool refresh, CollectionIndex coll) {
        if (_remoteCheckpointRequested[coll])
            return;     // already in progress
        if (!_remoteCheckpointDocID[coll])
            _remoteCheckpointDocID[coll] = _checkpointer[coll]->initialCheckpointID();
        if (!_remoteCheckpointDocID[coll] || _connectionState != Connection::kConnected)
            return;     // not ready yet

        logVerbose("Requesting remote checkpoint '%.*s' (collection: %u)",
                   SPLAT(_remoteCheckpointDocID[coll]), coll);
        MessageBuilder msg("getCheckpoint"_sl);
        msg["collection"_sl] = coll;
        msg["client"_sl] = _remoteCheckpointDocID[coll];
        Signpost::begin(Signpost::blipSent);
        sendRequest(msg, [this, refresh, coll](MessageProgress progress) {
            // ...after the checkpoint is received:
            if (progress.state != MessageProgress::kComplete)
                return;
            Signpost::end(Signpost::blipSent);
            MessageIn *response = progress.reply;

            auto collectionIn = response->intProperty("collection"_sl, kNotCollectionIndex);
            (void)collectionIn;
            // TBD: if collectionIn is not in the response, the server does not comply with the collection
            // protocol.
            DebugAssert(collectionIn == kNotCollectionIndex || collectionIn == coll);

            Checkpoint remoteCheckpoint;

            if (response->isError()) {
                auto err = response->getError();
                if (!(err.domain == "HTTP"_sl && err.code == 404))
                    return gotError(response);
                logInfo("No remote checkpoint '%.*s' (collection: %u)",
                        SPLAT(_remoteCheckpointDocID[coll]), coll);
                _remoteCheckpointRevID[coll].reset();
            } else {
                remoteCheckpoint.readJSON(response->body());
                _remoteCheckpointRevID[coll] = response->property("rev"_sl);
                logInfo("Received remote checkpoint (rev='%.*s'): %.*s (collection: %u)",
                        SPLAT(_remoteCheckpointRevID[coll]), SPLAT(response->body()), coll);
            }
            _remoteCheckpointReceived[coll] = true;

            if (!refresh && _hadLocalCheckpoint[coll]) {
                // Compare checkpoints, reset if mismatched:
                bool valid = _checkpointer[coll]->validateWith(remoteCheckpoint);
                if (!valid && _pushers[coll])
                    _pushers[coll]->checkpointIsInvalid();

                // Now we have the checkpoints! Time to start replicating:
                startReplicating(coll);
            }

            if (_checkpointJSONToSave[coll])
                saveCheckpointNow(coll);    // _saveCheckpoint() was waiting for _remoteCheckpointRevID
        });

        _remoteCheckpointRequested[coll] = true;

        // If there's no local checkpoint, we know we're starting from zero and don't need to
        // wait for the remote one before getting started:
        if (!refresh && !_hadLocalCheckpoint[coll])
            startReplicating(coll);
    }

    // getCollections() will be called when the replicator starts (start() or _onConnect())
    // when the _collections doesn't contain only the default collection. Otherwise,
    // getRemoteCheckpoint() will be called so that the replicator could work with the
    // pre-collection SG or CBL (P2P).
    void Replicator::getCollections() {
        if (_getCollectionsRequested)
            return;     // already in progress
        
        for (int i = 0; i < _collections.size(); i++) {
            if (!_remoteCheckpointDocID[i])
                _remoteCheckpointDocID[i] = _checkpointer[i]->initialCheckpointID();
            
            // Note: is there a case that _remoteCheckpointDocID[i] is nullslice?
            if (!_remoteCheckpointDocID[i] || _connectionState != Connection::kConnected)
                return;     // Not ready yet; Will be called again from _onConnect.
        }
        
        logVerbose("Requesting get collections");
        
        MessageBuilder msg("getCollections"_sl);
        auto &enc = msg.jsonBody();
        enc.beginDict();
        enc.writeKey("checkpoint_ids"_sl);
        enc.beginArray();
        for (int i = 0; i < _collections.size(); i++) {
            enc.writeString(_remoteCheckpointDocID[i]);
        }
        enc.endArray();
        enc.writeKey("collections"_sl);
        enc.beginArray();
        for (auto it = _collections.begin(); it != _collections.end(); ++it) {
            auto spec = it->get()->getSpec();
            auto collPath = Options::collectionSpecToPath(spec);
            enc.writeString(collPath);
        }
        enc.endArray();
        enc.endDict();
        
        Signpost::begin(Signpost::blipSent);
        sendRequest(msg, [this](MessageProgress progress) {
            // ...after the checkpoint is received:
            if (progress.state != MessageProgress::kComplete)
                return;
            Signpost::end(Signpost::blipSent);
            MessageIn *response = progress.reply;
            
            if (response->isError()) {
                return gotError(response);
            } else {
                alloc_slice json = response->body();
                Doc root = Doc::fromJSON(json, nullptr);
                if (!root) {
                    auto error = C4Error::printf(LiteCoreDomain, kC4ErrorRemoteError,
                                                 "Unparseable checkpoints: %.*s", SPLAT(json));
                    return gotError(error);
                }
                
                Array checkpointArray = root.asArray();
                if (checkpointArray.count() != _collections.size()) {
                    auto error = C4Error::printf(LiteCoreDomain, kC4ErrorRemoteError,
                                                 "Invalid number of checkpoints: %.*s", SPLAT(json));
                    return gotError(error);
                }
                
                // Validate and read each checkpoints:
                vector<Checkpoint> remoteCheckpoints(_collections.size());
                for (int i = 0; i < _collections.size(); i++) {
                    auto spec = _collections[i]->getSpec();
                    auto collPath = Options::collectionSpecToPath(spec);
                    
                    Dict dict = checkpointArray[i].asDict();
                    if (!dict) {
                        auto error = C4Error::printf(LiteCoreDomain, kC4ErrorNotFound,
                                                     "Collection '%.*s' is not found on the remote server",
                                                     SPLAT(collPath));
                        return gotError(error);
                    }
                    
                    if (dict.empty()) {
                        logInfo("No remote checkpoint '%.*s' for collection '%.*s'",
                                SPLAT(_remoteCheckpointDocID[i]), SPLAT(collPath));
                        _remoteCheckpointRevID[i].reset();
                    } else {
                        remoteCheckpoints[i].readDict(dict);
                        _remoteCheckpointRevID[i] = dict["rev"].asString();
                        logInfo("Received remote checkpoint (rev='%.*s') for collection '%.*s': %.*s",
                                SPLAT(_remoteCheckpointRevID[i]), SPLAT(collPath), SPLAT(dict.toString()));
                    }
                }
                
                for (int i = 0; i < _collections.size(); i++) {
                    _remoteCheckpointReceived[i] = true;
                    
                    if (_hadLocalCheckpoint[i]) {
                        // Compare checkpoints, reset if mismatched:
                        bool valid = _checkpointer[i]->validateWith(remoteCheckpoints[i]);
                        if (!valid && _pushers[i])
                            _pushers[i]->checkpointIsInvalid();

                        // Now we have the checkpoints! Time to start replicating:
                        startReplicating(i);
                    }

                    if (_checkpointJSONToSave[i])
                        saveCheckpointNow(i);    // _saveCheckpoint() was waiting for _remoteCheckpointRevID
                }
            }
        });

        _getCollectionsRequested = true;
        
        for (int i = 0; i < _collections.size(); i++) {
            if (!_hadLocalCheckpoint[i]) {
                startReplicating(i);
            }
        }
    }


    void Replicator::_saveCheckpoint(CollectionIndex coll, alloc_slice json) {
        if (!connected())
            return;
        _checkpointJSONToSave[coll] = move(json);
        if (_remoteCheckpointReceived[coll])
            saveCheckpointNow(coll);
        // ...else wait until checkpoint received (see above), which will call saveCheckpointNow().
    }


    void Replicator::saveCheckpointNow(CollectionIndex coll) {
        // Switch to the permanent checkpoint ID:
        alloc_slice checkpointID = _checkpointer[coll]->checkpointID();
        if (checkpointID != _remoteCheckpointDocID[coll]) {
            _remoteCheckpointDocID[coll] = checkpointID;
            _remoteCheckpointRevID[coll] = nullslice;
        }

        alloc_slice json = move(_checkpointJSONToSave[coll]);

        logVerbose("Saving remote checkpoint '%.*s' over rev='%.*s': %.*s (collection: %u) ...",
                   SPLAT(_remoteCheckpointDocID[coll]), SPLAT(_remoteCheckpointRevID[coll]),
                                                              SPLAT(json), coll);
        Assert(_remoteCheckpointReceived[coll]);
        Assert(json);

        MessageBuilder msg("setCheckpoint"_sl);
        msg["collection"_sl] = coll;
        msg["client"_sl] = _remoteCheckpointDocID[coll];
        msg["rev"_sl] = _remoteCheckpointRevID[coll];
        msg << json;
        Signpost::begin(Signpost::blipSent);
        sendRequest(msg, [=](MessageProgress progress) {
            if (progress.state != MessageProgress::kComplete)
                return;
            Signpost::end(Signpost::blipSent);
            MessageIn *response = progress.reply;

            auto collectionIn = response->intProperty("collection"_sl, kNotCollectionIndex);
            (void)collectionIn;
            // TBD: if collectionIn is not in the response, the server does not comply with the collection
            // protocol.
            DebugAssert(collectionIn == kNotCollectionIndex || collectionIn == coll);

            if (response->isError()) {
                Error responseErr = response->getError();
                if (responseErr.domain == "HTTP"_sl && responseErr.code == 409) {
                    // On conflict, read the remote checkpoint to get the real revID:
                    _checkpointJSONToSave[coll] = json; // move() has no effect here
                    _remoteCheckpointRequested[coll] = _remoteCheckpointReceived[coll] = false;
                    getRemoteCheckpoint(true, coll);
                } else {
                    gotError(response);
                    warn("Failed to save remote checkpoint (collection: %u)!", coll);
                    // If the checkpoint didn't save, something's wrong; but if we don't mark it as
                    // saved, the replicator will stay busy (see computeActivityLevel, line 169).
                    _checkpointer[coll]->saveCompleted();
                }
            } else {
                // Remote checkpoint saved, so update local one:
                _remoteCheckpointRevID[coll] = response->property("rev"_sl);
                logInfo("Saved remote checkpoint '%.*s' as rev='%.*s' (collection: %u)",
                    SPLAT(_remoteCheckpointDocID[coll]), SPLAT(_remoteCheckpointRevID[coll]), coll);

                try {
                    _db->useLocked([&](C4Database *db) {
                        _db->markRevsSyncedNow();
                        _checkpointer[coll]->write(db, json);
                    });
                    logInfo("Saved local checkpoint '%.*s': %.*s (collection: %u)",
                            SPLAT(_remoteCheckpointDocID[coll]), SPLAT(json), coll);
                } catch (...) {
                    gotError(C4Error::fromCurrentException());
                }
                _checkpointer[coll]->saveCompleted();
            }
        });
    }


    bool Replicator::pendingDocumentIDs(Checkpointer::PendingDocCallback callback){
        // CBL-2448
        auto db = _db;
        if(!db) {
            return false;
        }

        try {
            db->useLocked([this, callback](const Retained<C4Database>& db) {
                // TBD: replacing 0 with real collection index
                _checkpointer[0]->pendingDocumentIDs(db, callback);
            });
            return true;
        } catch (const error& err) {
            if (error{error::Domain::LiteCore, error::LiteCoreError::NotOpen} == err) {
                return false;
            } else {
                throw;
            }
        }
    }


    optional<bool> Replicator::isDocumentPending(slice docID) {
        // CBL-2448
        auto db = _db;
        if(!db) {
            return nullopt;
        }
        // TBD: docID is not enough to idendtify doc. Need collection
        CollectionIndex coll = 0;
        try {
            return db->useLocked<bool>([this,docID, coll](const Retained<C4Database>&db) {
                return _checkpointer[coll]->isDocumentPending(db, docID);
            });
        } catch(const error& err) {
            if (error{error::Domain::LiteCore, error::LiteCoreError::NotOpen} == err) {
                return nullopt;
            } else {
                throw;
            }
        }
    }


#pragma mark - PEER CHECKPOINT ACCESS:


    // Gets the ID from a checkpoint request
    slice Replicator::getPeerCheckpointDocID(MessageIn* request, const char *whatFor) const {
        slice checkpointID = request->property("client"_sl);
        if (checkpointID)
            logInfo("Request to %s peer checkpoint '%.*s'", whatFor, SPLAT(checkpointID));
        else
            request->respondWithError({"BLIP"_sl, 400, "missing checkpoint ID"_sl});
        return checkpointID;
    }


    // Handles a "getCheckpoint" request by looking up a peer checkpoint.
    void Replicator::handleGetCheckpoint(Retained<MessageIn> request) {
        slice checkpointID = getPeerCheckpointDocID(request, "get");
        if (!checkpointID)
            return;
        
        alloc_slice body, revID;
        int status = 0;
        try {
            if (!Checkpointer::getPeerCheckpoint(_db->useLocked(), checkpointID, body, revID))
                status = 404;
        } catch (...) {
            C4Error::warnCurrentException("Replicator::handleGetCheckpoint");
            status = 502;
        }

        if (status != 0) {
            request->respondWithError({"HTTP"_sl, status});
            return;
        }

        MessageBuilder response(request);
        response["rev"_sl] = revID;
        response << body;
        request->respond(response);
    }


    // Handles a "setCheckpoint" request by storing a peer checkpoint.
    void Replicator::handleSetCheckpoint(Retained<MessageIn> request) {
        slice checkpointID = getPeerCheckpointDocID(request, "set");
        if (!checkpointID)
            return;

        bool ok;
        alloc_slice newRevID;
        try {
            ok = Checkpointer::savePeerCheckpoint(_db->useLocked(),
                                                  checkpointID,
                                                  request->body(),
                                                  request->property("rev"_sl),
                                                  newRevID);
        } catch (...) {
            request->respondWithError(c4ToBLIPError(C4Error::fromCurrentException()));
            return;
        }

        if (!ok) {
            request->respondWithError({"HTTP"_sl, 409, alloc_slice("revision ID mismatch"_sl)});
            return;
        }

        MessageBuilder response(request);
        response["rev"_sl] = newRevID;
        request->respond(response);
    }


    // Handles a "getCollections" request by looking up a peer checkpoint of each collection.
    void Replicator::handleGetCollections(Retained<blip::MessageIn> request) {
        auto root = request->JSONBody().asDict();
        if (!root) {
            request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: no root"_sl});
            return;
        }
        
        auto checkpointIDs = root["checkpoint_ids"].asArray();
        if (!checkpointIDs || checkpointIDs.empty()) {
            request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: no checkpoint_ids"_sl});
            return;
        }
        
        auto collections = root["collections"].asArray();
        if (!collections || collections.empty()) {
            request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: no collections"_sl});
            return;
        }
        
        if (checkpointIDs.count() != collections.count()) {
            request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: mismatched checkpoint_ids and collections"_sl});
            return;
        }
        
        unordered_set<C4Database::CollectionSpec> specs;
        for (auto &coll : _collections) {
            specs.insert(coll->getSpec());
        }
        
        MessageBuilder response(request);
        auto &enc = response.jsonBody();
        enc.beginArray();
        
        for (int i = 0; i < checkpointIDs.count(); i++) {
            auto checkpointID = checkpointIDs[i].asString();
            auto collectionPath = collections[i].asString();
            
            logInfo("Request to get peer checkpoint '%.*s' for collection '%.*s'",
                    SPLAT(checkpointID), SPLAT(collectionPath));
            
            C4Database::CollectionSpec spec = Options::collectionPathToSpec(collectionPath);
            if (specs.find(spec) == specs.end()) {
                enc.writeNull();
                continue;
            }
            
            alloc_slice body, revID;
            int status = 0;
            try {
                if (!Checkpointer::getPeerCheckpoint(_db->useLocked(), checkpointID, body, revID)) {
                    enc.writeValue(Dict::emptyDict());
                    continue;
                }
            } catch (...) {
                C4Error::warnCurrentException("Replicator::handleGetCollections");
                status = 502;
            }
            
            if (status != 0) {
                request->respondWithError({"HTTP"_sl, status});
                return;
            }
            
            Doc doc(body);
            auto checkpoint = doc.root().asDict().mutableCopy();
            checkpoint.set("rev"_sl, revID);
            enc.writeValue(checkpoint);
        }
        enc.endArray();
        request->respond(response);
    }

} }
