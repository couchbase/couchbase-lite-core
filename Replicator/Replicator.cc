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

#include <memory>

#include <utility>

#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "Pusher.hh"
#include "Puller.hh"
#include "Checkpoint.hh"
#include "DBAccess.hh"
#include "DatabaseImpl.hh"
#include "Delimiter.hh"
#include "c4Database.hh"
#include "c4DocEnumerator.hh"
#include "c4SocketTypes.h"  // for error codes
#include "Error.hh"
#include "StringUtil.hh"
#include "Logging.hh"
#include "Headers.hh"
#include "Instrumentation.hh"
#include "fleece/Mutable.hh"

using namespace std;
using namespace std::placeholders;
using namespace fleece;
using namespace litecore::blip;

namespace litecore::repl {
    struct StoppingErrorEntry {
        // NOLINT(cppcoreguidelines-pro-type-member-init)
        C4Error err;
        bool    isFatal;
        slice   msg;
    };

    // Errors treated specially by onError()
    static constexpr StoppingErrorEntry StoppingErrors[] = {
            {{LiteCoreDomain, kC4ErrorUnexpectedError, 0}, true, "An exception was thrown"_sl},
            {{WebSocketDomain, 400, 0}, true, "Unrecognized collection"_sl},
            {{WebSocketDomain, 403, 0}, true, "An attempt was made to perform an unauthorized action"_sl},
            {{WebSocketDomain, 503, 0}, false, "The server is over capacity"_sl},
            {{LiteCoreDomain, kC4ErrorRemoteError, 0}, true, "Unexpected error from remote"_sl}};

    std::string Replicator::ProtocolName() {
        stringstream result;
        delimiter    delim(",");
        for ( auto& name : kCompatProtocols ) result << delim << name;
        return result.str();
    }

    Replicator::Replicator(C4Database* db, websocket::WebSocket* webSocket, Delegate& delegate, Options* options)
        : Replicator(make_shared<DBAccess>(db, options->properties["disable_blob_support"_sl].asBool()), webSocket,
                     delegate, options) {}

    Replicator::Replicator(const shared_ptr<DBAccess>& db, websocket::WebSocket* webSocket, Delegate& delegate,
                           Options* options)
        : Worker(new Connection(webSocket, options->properties, {}), nullptr, options, db, "Repl", kNotCollectionIndex)
        , _delegate(&delegate)
        , _connectionState(connection().state())
        , _docsEnded(this, "docsEnded", &Replicator::notifyEndedDocuments, tuning::kMinDocEndedInterval, 100) {
        try {
            connection().setParentObjectRef(getObjectRef());
            db->setParentObjectRef(getObjectRef());

            // Post-conditions:
            //   collectionOpts.size() > 0
            //   collectionAware == false if and only if collectionOpts.size() == 1 &&
            //                                           collectionOpts[0].collectionPath == defaultCollectionPath
            //   isActive == true ? all collections are active
            //                    : all collections are passive.
            _options->verify();

            _loggingID  = string(db->useLocked()->getPath()) + " " + _loggingID;
            _importance = 2;

            string logName = db->useLocked<std::string>([](const C4Database* db) {
                DatabaseImpl* impl = asInternal(db);
                return impl->dataFile()->loggingName();
            });
            logInfo("DB=%s Instantiated %s", logName.c_str(), string(*options).c_str());

#ifdef LITECORE_CPPTEST
            _delayChangesResponse   = _options->delayChangesResponse();
            _disableReplacementRevs = _options->disableReplacementRevs();
#endif

            _remoteURL = webSocket->url();
            if ( _options->isActive() ) { prepareWorkers(); }

            // Following messages are handled by appropriate workers.
            // Replicator receives all the messages. Based on collectionIndex,
            // it dispatches the message to appropriate workers.
            for ( auto profile : {
                          "subChanges", "getAttachment", "proveAttachment",  // passive pushers
                          "changes", "proposeChanges", "rev", "norev"        // passive pullers
                  } ) {
                registerHandler(profile, &Replicator::delegateCollectionSpecificMessageToWorker);
            }

            registerHandler("getCheckpoint", &Replicator::handleGetCheckpoint);
            registerHandler("setCheckpoint", &Replicator::handleSetCheckpoint);
            registerHandler("getCollections", &Replicator::handleGetCollections);
        } catch ( ... ) {
            // terminate to break the circular references: connection -> BLIPIO -> connection.
            terminate();
            throw;
        }
    }

    void Replicator::start(bool reset, bool synchronous) {
        if ( synchronous ) _start(reset);
        else
            enqueue(FUNCTION_TO_QUEUE(Replicator::_start), reset);
    }

    void Replicator::_start(bool reset) {
        try {
            Assert(_connectionState == Connection::kClosed);
            Signpost::begin(Signpost::replication, uintptr_t(this));
            _connectionState = Connection::kConnecting;

            // _start'ed Replicator must be _onClose'ed
            _weakConnectionDelegateThis = new WeakHolder<blip::ConnectionDelegate>(this);
            connection().start(_weakConnectionDelegateThis);
            // Now wait for _onConnect or _onClose...

            if ( !_options->isActive() ) { return; }

            _findExistingConflicts();

            bool goOn = true;
            for ( CollectionIndex i = 0; goOn && i < _subRepls.size(); ++i ) {
                // if any getLocalCheckpoint fails, the replicator would already be stopped.
                goOn = goOn && getLocalCheckpoint(reset, i);
            }
            if ( goOn ) {
                if ( _options->collectionAware() ) {
                    getCollections();
                } else {
                    getRemoteCheckpoint(false, 0);
                }
            }
        } catch ( ... ) {
            C4Error err = C4Error::fromCurrentException();
            logError("Failed to start replicator: %s", err.description().c_str());
            gotError(err);
            stop();
            _weakConnectionDelegateThis = nullptr;
        }
    }

    void Replicator::_findExistingConflicts() {
        // Active replicator
        Stopwatch st;
        for ( CollectionIndex i = 0; i < _subRepls.size(); ++i ) {
            SubReplicator& sub = _subRepls[i];
            try {
                unique_ptr<C4DocEnumerator> e = _db->unresolvedDocsEnumerator(sub.collection, false);
                cLogInfo(i, "Scanning for pre-existing conflicts...");
                unsigned nConflicts = 0;
                while ( e->next() ) {
                    C4DocumentInfo info = e->documentInfo();
                    auto           rev  = retained(new RevToInsert(nullptr,                           /* incoming rev */
                                                                   info.docID, info.revID, nullslice, /* history buf */
                                                                   info.flags & kDocDeleted, false, sub.collection->getSpec(),
                                                                   _options->collectionCallbackContext(i)));
                    rev->error          = C4Error::make(LiteCoreDomain, kC4ErrorConflict);
                    _docsEnded.push(rev);
                    ++nConflicts;
                }
                cLogInfo(i, "Found %u conflicted docs in %.3f sec", nConflicts, st.elapsed());
            } catch ( ... ) {
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
        if ( connected() ) {
            logDebug("...connected() was true, doing extra stuff...");
            Assert(_connectionState == Connection::kClosed);
            connection().terminate();
            std::for_each(_subRepls.begin(), _subRepls.end(), [](SubReplicator& sub) {
                sub.pusher = nullptr;
                sub.puller = nullptr;
            });
            _workerHandlers.clear();
        }

        // CBL-1061: This used to be inside the connected(), but static analysis shows
        // terminate() is only called from the destructor of the _delegate itself, so it is
        // dangerous to leave it around.  Set it to null here to avoid using it further.
        _delegate = nullptr;
        _db.reset();
        logDebug("...done with terminate()");
    }

    void Replicator::_disconnect(websocket::CloseCode closeCode, slice message) {
        if ( connected() ) {
            connection().close(closeCode, message);
            _connectionState = Connection::kClosing;
        }
    }

    // Called after the checkpoint is established.
    void Replicator::startReplicating(CollectionIndex coll) {
        if ( _options->push(coll) > kC4Passive ) _subRepls[coll].pusher->start();
        if ( _options->pull(coll) > kC4Passive )
            _subRepls[coll].puller->start(_subRepls[coll].checkpointer->remoteMinSequence());
    }

    void Replicator::docRemoteAncestorChanged(alloc_slice docID, alloc_slice revID, CollectionIndex coll) {
        Retained<Pusher> pusher = _subRepls[coll].pusher;
        if ( pusher ) pusher->docRemoteAncestorChanged(std::move(docID), std::move(revID));
    }

    void Replicator::returnForbidden(Retained<blip::MessageIn> request) {
        auto            collectionIn = request->intProperty(kCollectionProperty, kNotCollectionIndex);
        CollectionIndex c            = 0;
        if ( collectionIn != kNotCollectionIndex ) {
            c = (CollectionIndex)collectionIn;
        } else {
            warn("\"collection\" property is not present in the request; 0 is used");
        }
        if ( _options->push(c) != kC4Disabled ) {
            request->respondWithError(Error("HTTP"_sl, 403, "Attempting to push to a pull-only replicator"_sl));
        } else {
            request->respondWithError(Error("HTTP"_sl, 403, "Attempting to pull from a push-only replicator"_sl));
        }
    }

#pragma mark - STATUS:

    string Replicator::statusVString() const {
        std::stringstream ss;
        for ( CollectionIndex i = 0; i < _subRepls.size(); ++i ) {
            const SubReplicator& sub = _subRepls[i];
            if ( i > 0 ) { ss << '|'; }
            ss << "pushStatus=" << kC4ReplicatorActivityLevelNames[sub.pushStatus.level];
            ss << ", pullStatus=" << kC4ReplicatorActivityLevelNames[sub.pullStatus.level];
            ss << ", progress=" << sub.pushStatus.progress.unitsCompleted + sub.pullStatus.progress.unitsCompleted;
            ss << "/";
            ss << sub.pushStatus.progress.unitsTotal + sub.pullStatus.progress.unitsTotal;
            ss << ", docCount=" << sub.pushStatus.progress.documentCount + sub.pullStatus.progress.documentCount;
        }
        return ss.str();
    }

    void Replicator::updatePushStatus(CollectionIndex i, const Status& status) {
        SubReplicator& sub = _subRepls[i];

        // Status::level
        if ( status.level >= _pushStatus.level ) {
            sub.pushStatus.level = status.level;
            _pushStatus.level    = status.level;
        } else {
            auto prevLevel       = sub.pushStatus.level;
            sub.pushStatus.level = status.level;
            if ( prevLevel >= _pushStatus.level ) {
                auto it           = std::max_element(_subRepls.begin(), _subRepls.end(),
                                                     [](const SubReplicator& a, const SubReplicator& b) {
                                               return a.pushStatus.level < b.pushStatus.level;
                                           });
                _pushStatus.level = it->pushStatus.level;
            }
        }

        // Status::progress
        auto delta              = status.progress - sub.pushStatus.progress;
        sub.pushStatus.progress = status.progress;
        _pushStatus.progress += delta;

        // Status::error
        sub.pushStatus.error = status.error;
        // overall error moves from 0 to non-0.
        if ( _pushStatus.error.code == 0 ||
             // transient error is superceded by non-transient error.
             (_pushStatus.error.mayBeTransient() && !status.error.mayBeTransient()) ) {
            _pushStatus.error = status.error;
        }
    }

    void Replicator::updatePullStatus(CollectionIndex i, const Status& status) {
        SubReplicator& sub = _subRepls[i];
        // Status::level
        if ( status.level >= _pullStatus.level ) {
            sub.pullStatus.level = status.level;
            _pullStatus.level    = status.level;
        } else {
            auto prevLevel       = sub.pullStatus.level;
            sub.pullStatus.level = status.level;
            if ( prevLevel >= _pullStatus.level ) {
                auto it           = std::max_element(_subRepls.begin(), _subRepls.end(),
                                                     [](const SubReplicator& a, const SubReplicator& b) {
                                               return a.pullStatus.level < b.pullStatus.level;
                                           });
                _pullStatus.level = it->pullStatus.level;
            }
        }

        // Status::progress
        auto delta              = status.progress - sub.pullStatus.progress;
        sub.pullStatus.progress = status.progress;
        _pullStatus.progress += delta;

        // Status::error
        sub.pullStatus.error = status.error;
        // overall error moves from 0 to non-0.
        if ( _pullStatus.error.code == 0 ||
             // transient error is superceded by non-transient error.
             (_pullStatus.error.mayBeTransient() && !status.error.mayBeTransient()) ) {
            _pullStatus.error = status.error;
        }
    }

    // The status of one of the actors has changed; update mine
    void Replicator::_childChangedStatus(Retained<Worker> task, Status taskStatus) {
        if ( status().level == kC4Stopped )  // I've already stopped & cleared refs; ignore this
            return;

        CollectionIndex coll = task->collectionIndex();
        if ( coll != kNotCollectionIndex ) {
            if ( task == _subRepls[coll].pusher ) {
                updatePushStatus(coll, taskStatus);
            } else if ( task == _subRepls[coll].puller ) {
                updatePullStatus(coll, taskStatus);
            }
        }

        setProgress(_pushStatus.progress + _pullStatus.progress);

        if ( SyncBusyLog.willLog(LogLevel::Info) ) {
            logInfo("pushStatus=%-s, pullStatus=%-s, progress=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "",
                    kC4ReplicatorActivityLevelNames[_pushStatus.level],
                    kC4ReplicatorActivityLevelNames[_pullStatus.level], status().progress.unitsCompleted,
                    status().progress.unitsTotal, status().progress.documentCount);
        }
        if ( SyncBusyLog.willLog(LogLevel::Verbose) ) {
            logVerbose("Replicator status collection-wise: %s", statusVString().c_str());
        }

        if ( _pullStatus.error.code ) onError(_pullStatus.error);
        else if ( _pushStatus.error.code )
            onError(_pushStatus.error);

        if ( coll != kNotCollectionIndex ) {
            // Save a checkpoint immediately when push or pull finishes or goes idle:
            if ( taskStatus.level == kC4Stopped || taskStatus.level == kC4Idle ) _subRepls[coll].checkpointer->save();
        }
    }

    Worker::ActivityLevel Replicator::computeActivityLevel() const {
        // Once I've announced I've stopped, don't return any other status again:
        auto currentLevel = status().level;
        if ( currentLevel == kC4Stopped ) return kC4Stopped;

        ActivityLevel level      = kC4Busy;
        bool          hasUnsaved = false;
        switch ( _connectionState ) {
            case Connection::kConnecting:
                level = kC4Connecting;
                break;
            case Connection::kConnected:
                {
                    hasUnsaved = std::any_of(_subRepls.begin(), _subRepls.end(),
                                             [](const SubReplicator& sub) { return sub.checkpointer->isUnsaved(); });
                    if ( hasUnsaved ) level = kC4Busy;
                    else
                        level = Worker::computeActivityLevel();
                    level = max(level, max(_pushStatus.level, _pullStatus.level));
                    if ( level == kC4Idle && !isContinuous() && !isOpenServer() ) {
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
                if ( level < kC4Busy ) level = kC4Stopped;
                else if ( currentLevel == kC4Connecting )
                    level = kC4Connecting;
                break;
        }
        if ( SyncBusyLog.willLog(LogLevel::Info) ) {
            logInfo("activityLevel=%-s: connectionState=%d, savingChkpt=%d", kC4ReplicatorActivityLevelNames[level],
                    _connectionState, hasUnsaved);
        }
        return level;
    }

    void Replicator::onError(C4Error error) {
        if ( status().error.code != 0 && error.domain == WebSocketDomain
             && (error.code == kWebSocketCloseAppPermanent || error.code == kWebSocketCloseAppTransient) ) {
            // CBL-1178: If we already have an error code, it is more relevant than the web socket close code, so keep it
            // intact so that the consumer can know what went wrong
            logVerbose("kWebSocketCloseAppPermanent or kWebSocketCloseAppTransient received, ignoring (only relevant "
                       "for underlying connection...)");
            return;
        }

        Worker::onError(error);
        for ( const StoppingErrorEntry& stoppingErr : StoppingErrors ) {
            if ( stoppingErr.err == error ) {
                string message = error.description();
                if ( stoppingErr.isFatal ) {
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
        if ( status().level == kC4Stopped ) {
            DebugAssert(!connected());  // must already have gotten _onClose() delegate callback
            std::for_each(_subRepls.begin(), _subRepls.end(), [](SubReplicator& sub) {
                sub.pusher = nullptr;
                sub.puller = nullptr;
            });
            _workerHandlers.clear();
            _db->close();
            Signpost::end(Signpost::replication, uintptr_t(this));
        }
        if ( _delegate ) {
            // Notify the delegate of the current status, but not too often:
            auto waitFor = tuning::kMinDelegateCallInterval - _sinceDelegateCall.elapsedDuration();
            if ( waitFor <= 0s || status().level != _lastDelegateCallLevel ) {
                reportStatus();
            } else if ( !_waitingToCallDelegate ) {
                _waitingToCallDelegate = true;
                enqueueAfter(waitFor, FUNCTION_TO_QUEUE(Replicator::reportStatus));
            }
        }
    }

    void Replicator::reportStatus() {
        _waitingToCallDelegate = false;
        _lastDelegateCallLevel = status().level;
        _sinceDelegateCall.reset();
        if ( _delegate ) {
            notifyEndedDocuments();
            _delegate->replicatorStatusChanged(this, status());
        }
        if ( status().level == kC4Stopped ) _delegate = nullptr;  // Never call delegate after telling it I've stopped
    }

    void Replicator::endedDocument(ReplicatedRev* d) {
        logInfo("documentEnded %.*s %.*s flags=%02x (%d/%d)", SPLAT(d->docID), SPLAT(d->revID), d->flags,
                d->error.domain, d->error.code);
        d->trim();  // free up unneeded stuff
        if ( _delegate ) {
            if ( d->isWarning && (d->flags & kRevIsConflict) ) {
                // Inserter::insertRevisionNow set this flag to indicate that the rev caused a
                // conflict (though it did get inserted), so notify the delegate of the conflict:
                d->error            = C4Error::make(LiteCoreDomain, kC4ErrorConflict, nullslice);
                d->errorIsTransient = true;
            }
            _docsEnded.push(d);
        }
    }

    void Replicator::notifyEndedDocuments(int gen) {
        auto docs = _docsEnded.pop(gen);
        if ( docs && !docs->empty() && _delegate ) _delegate->replicatorDocumentsEnded(this, *docs);
    }

    void Replicator::_onBlobProgress(BlobProgress p) {
        if ( _delegate ) _delegate->replicatorBlobProgress(this, p);
    }

#pragma mark - BLIP DELEGATE:

    void Replicator::onTLSCertificate(slice certData) {
        if ( _delegate ) _delegate->replicatorGotTLSCertificate(certData);
    }

    void Replicator::onHTTPResponse(int status, const websocket::Headers& headers) {
        enqueue(FUNCTION_TO_QUEUE(Replicator::_onHTTPResponse), status, headers);
    }

    void Replicator::_onHTTPResponse(int status, websocket::Headers headers) {
        if ( status == 101 && !headers["Sec-WebSocket-Protocol"_sl] ) {
            gotError(C4Error::make(WebSocketDomain, kWebSocketCloseProtocolError,
                                   "Incompatible replication protocol "
                                   "(missing 'Sec-WebSocket-Protocol' response header)"_sl));
        }
        if ( _delegate ) _delegate->replicatorGotHTTPResponse(this, status, headers);
    }

    void Replicator::_onConnect() {
        logInfo("Connected!");
        Signpost::mark(Signpost::replicatorConnect, uintptr_t(this));
        if ( _connectionState != Connection::kClosing ) {
            // skip this if stop() already called
            _connectionState = Connection::kConnected;
            if ( _options->isActive() ) {
                if ( _options->collectionAware() ) {
                    getCollections();
                } else {
                    getRemoteCheckpoint(false, 0);
                }
            }
        }
    }

    void Replicator::_onClose(Connection::CloseStatus status, Connection::State state) {
        logInfo("Connection closed with %-s %d: \"%.*s\" (state=%d->%d)", status.reasonName(), status.code,
                SPLAT(status.message), _connectionState, state);
        Signpost::mark(Signpost::replicatorDisconnect, uintptr_t(this));

        bool closedByPeer = (_connectionState != Connection::kClosing);
        _connectionState  = state;

        std::for_each(_subRepls.begin(), _subRepls.end(), [](SubReplicator& sub) { sub.checkpointer->stopAutosave(); });

        // Clear connection() and notify the other agents to do the same:
        _connectionClosed();
        for ( auto& sub : _subRepls ) {
            if ( sub.pusher ) sub.pusher->connectionClosed();
            if ( sub.puller ) sub.puller->connectionClosed();
        }

        if ( status.isNormal() && closedByPeer && _options->isActive() ) {
            logInfo("I didn't initiate the close; treating this as code 1001 (GoingAway)");
            status.code    = websocket::kCodeGoingAway;
            status.message = alloc_slice("WebSocket connection closed by peer");
        }

        static const C4ErrorDomain kDomainForReason[] = {WebSocketDomain, POSIXDomain, NetworkDomain, LiteCoreDomain};

        // If this was an unclean close, set my error property:
        if ( status.reason != websocket::kWebSocketClose || status.code != websocket::kCodeNormal ) {
            int           code = status.code;
            C4ErrorDomain domain;
            if ( status.reason < sizeof(kDomainForReason) / sizeof(C4ErrorDomain) ) {
                domain = kDomainForReason[status.reason];
            } else {
                domain = LiteCoreDomain;
                code   = kC4ErrorRemoteError;
            }
            gotError(C4Error::make(domain, code, status.message));
        }

        if ( _delegate ) {
            notifyEndedDocuments();
            _delegate->replicatorConnectionClosed(this, status);
        }
        _weakConnectionDelegateThis = nullptr;
    }

    // This only gets called if none of the registered handlers were triggered.
    void Replicator::_onRequestReceived(Retained<MessageIn> msg) {
        auto collection = (CollectionIndex)msg->intProperty(kCollectionProperty, kNotCollectionIndex);
        warn("Received unrecognized BLIP request #%" PRIu64 "(collection: %u) with Profile '%.*s', %zu bytes",
             msg->number(), collection, SPLAT(msg->property("Profile"_sl)), msg->body().size);
        msg->notHandled();
    }

#pragma mark - CHECKPOINT:

    // Start off by getting the local checkpoint, if this is an active replicator:
    bool Replicator::getLocalCheckpoint(bool reset, CollectionIndex coll) {
        SubReplicator& sub = _subRepls[coll];
        try {
            if ( sub.checkpointer->read(_db->useLocked(), reset) ) {
                auto remote = sub.checkpointer->remoteMinSequence();
                cLogInfo(coll, "Read local checkpoint '%.*s': %.*s", SPLAT(sub.checkpointer->initialCheckpointID()),
                         SPLAT(sub.checkpointer->checkpointJSON()));
                sub.hadLocalCheckpoint = true;
            } else if ( reset ) {
                cLogInfo(coll, "Ignoring local checkpoint ('reset' option is set)");
            } else {
                cLogInfo(coll, "No local checkpoint '%.*s'", SPLAT(sub.checkpointer->initialCheckpointID()));
                // If pulling into an empty db with no checkpoint, it's safe to skip deleted
                // revisions as an optimization.
                if ( _options->pull(coll) > kC4Passive && sub.puller
                     && _db->useCollection(sub.collection)->getLastSequence() == 0_seq )
                    sub.puller->setSkipDeleted();
            }
            return true;
        } catch ( ... ) {
            cLogInfo(coll, "Fatal error getting local checkpoint");
            gotError(C4Error::fromCurrentException());
            stop();
            return false;
        }
    }

    // Get the remote checkpoint, after we've got the local one and the BLIP connection is up.
    void Replicator::getRemoteCheckpoint(bool refresh, CollectionIndex coll) {
        SubReplicator& sub = _subRepls[coll];
        if ( sub.remoteCheckpointRequested ) return;  // already in progress
        if ( !sub.remoteCheckpointDocID ) sub.remoteCheckpointDocID = sub.checkpointer->initialCheckpointID();
        if ( !sub.remoteCheckpointDocID || _connectionState != Connection::kConnected ) return;  // not ready yet

        if ( !_options->collectionAware() ) {
            logVerbose("Requesting remote checkpoint '%.*s' of the default collection",
                       SPLAT(sub.remoteCheckpointDocID));
        } else {
            cLogVerbose(coll, "Requesting remote checkpoint '%.*s'", SPLAT(sub.remoteCheckpointDocID));
        }
        MessageBuilder msg("getCheckpoint"_sl);
        msg["client"_sl] = sub.remoteCheckpointDocID;
        assignCollectionToMsg(msg, coll);
        Signpost::begin(Signpost::blipSent);
        sendRequest(msg, [this, refresh, coll, &sub](const MessageProgress& progress) {
            // ...after the checkpoint is received:
            if ( progress.state != MessageProgress::kComplete ) return;
            Signpost::end(Signpost::blipSent);
            MessageIn* response = progress.reply;
            Checkpoint remoteCheckpoint;

            if ( response->isError() ) {
                auto err = response->getError();
                if ( !(err.domain == "HTTP"_sl && err.code == 404) ) return gotError(response);
                if ( !_options->collectionAware() ) {
                    logInfo("No remote checkpoint '%.*s' of the default collection", SPLAT(sub.remoteCheckpointDocID));
                } else {
                    cLogInfo(coll, "No remote checkpoint '%.*s'", SPLAT(sub.remoteCheckpointRevID));
                }
                sub.remoteCheckpointRevID.reset();
            } else {
                remoteCheckpoint.readJSON(response->body());
                sub.remoteCheckpointRevID = response->property("rev"_sl);
                if ( !_options->collectionAware() ) {
                    logInfo("Received remote checkpoint (rev='%.*s'): %.*s of the default collection",
                            SPLAT(sub.remoteCheckpointRevID), SPLAT(response->body()));
                } else {
                    cLogInfo(coll, "Received remote checkpoint (rev='%.*s'): %.*s", SPLAT(sub.remoteCheckpointRevID),
                             SPLAT(response->body()));
                }
            }
            sub.remoteCheckpointReceived = true;

            if ( !refresh && sub.hadLocalCheckpoint ) {
                // Compare checkpoints, reset if mismatched:
                bool valid = sub.checkpointer->validateWith(remoteCheckpoint);
                if ( !valid && sub.pusher ) sub.pusher->checkpointIsInvalid();

                if ( !refresh ) {
                    // Now we have the checkpoints! Time to start replicating:
                    startReplicating(coll);
                }
            }

            if ( sub.checkpointJSONToSave ) saveCheckpointNow(coll);
            // _saveCheckpoint() was waiting for _remoteCheckpointRevID
        });

        sub.remoteCheckpointRequested = true;

        // If there's no local checkpoint, we know we're starting from zero and don't need to
        // wait for the remote one before getting started:
        if ( !refresh && !sub.hadLocalCheckpoint ) startReplicating(coll);
    }

    // getCollections() will be called when the replicator starts (start() or _onConnect())
    // when the _collections doesn't contain only the default collection. Otherwise,
    // getRemoteCheckpoint() will be called so that the replicator could work with the
    // pre-collection SG or CBL (P2P).
    void Replicator::getCollections() {
        if ( _getCollectionsRequested ) return;  // already in progress

        if ( _connectionState != Connection::kConnected ) return;
        // Not ready yet; Will be called again from _onConnect.

        for ( auto& _subRepl : _subRepls ) {
            if ( !_subRepl.remoteCheckpointDocID )
                _subRepl.remoteCheckpointDocID = _subRepl.checkpointer->initialCheckpointID();

            // Note:
            // This check is copied from getRemoteCheckpoint().
            // Is there a case that _remoteCheckpointDocID[i] is nullslice?
            if ( !_subRepl.remoteCheckpointDocID ) return;  // Not ready yet.
        }

        logVerbose("Requesting get collections");

        MessageBuilder msg("getCollections"_sl);
        auto&          enc = msg.jsonBody();
        enc.beginDict();
        enc.writeKey("checkpoint_ids"_sl);
        enc.beginArray();
        for ( auto& _subRepl : _subRepls ) { enc.writeString(_subRepl.remoteCheckpointDocID); }
        enc.endArray();
        enc.writeKey("collections"_sl);
        enc.beginArray();
        for ( int i = 0; i < _subRepls.size(); i++ ) { enc.writeString(_options->collectionPath(i)); }
        enc.endArray();
        enc.endDict();

        Signpost::begin(Signpost::blipSent);
        sendRequest(msg, [this](const MessageProgress& progress) {
            // ...after the checkpoint is received:
            if ( progress.state != MessageProgress::kComplete ) return;
            Signpost::end(Signpost::blipSent);
            MessageIn* response = progress.reply;

            if ( response->isError() ) {
                gotError(response);
                if ( auto err = response->getError(); err.domain == "BLIP"_sl && err.code == 404 ) {
                    constexpr slice sg3_0_specific                 = "No handler for BLIP request"_sl;
                    constexpr slice lite3_0_specific               = "no handler for message"_sl;
                    enum { kUnknown, kSyncGwy, kP2PRemote } remote = kUnknown;
                    if ( err.message == sg3_0_specific ) {
                        remote = kSyncGwy;
                    } else if ( err.message == lite3_0_specific ) {
                        remote = kP2PRemote;
                    }
                    if ( remote != kUnknown ) {
                        logError("%s%s", remote == kSyncGwy ? "This Sync Gateway" : "This Remote Peer",
                                 " does not support named collections. Try configuring your replicator using the "
                                 "default collection");
                    }
                }
                return;
            } else {
                alloc_slice json = response->body();
                Doc         root = Doc::fromJSON(json, nullptr);
                if ( !root ) {
                    auto error = C4Error::printf(LiteCoreDomain, kC4ErrorRemoteError, "Unparseable checkpoints: %.*s",
                                                 SPLAT(json));
                    return gotError(error);
                }

                Array checkpointArray = root.asArray();
                if ( checkpointArray.count() != _subRepls.size() ) {
                    auto error = C4Error::printf(LiteCoreDomain, kC4ErrorRemoteError,
                                                 "Invalid number of checkpoints: %.*s", SPLAT(json));
                    return gotError(error);
                }

                // Validate and read each checkpoints:
                vector<Checkpoint> remoteCheckpoints(_subRepls.size());
                for ( int i = 0; i < _subRepls.size(); i++ ) {
                    auto           collPath = _options->collectionPath(i);
                    SubReplicator& sub      = _subRepls[i];
                    Dict           dict     = checkpointArray[i].asDict();
                    if ( !dict ) {
                        // Make it fatal error, UnexpectedError.
                        auto error =
                                C4Error::printf(WebSocketDomain, 404,
                                                "Collection '%.*s' is not found on the remote server", SPLAT(collPath));
                        gotError(error);
                        _stop();
                        return;
                    }

                    if ( dict.empty() ) {
                        cLogInfo(i, "No remote checkpoint '%.*s'", SPLAT(sub.remoteCheckpointDocID));
                        sub.remoteCheckpointRevID.reset();
                    } else {
                        remoteCheckpoints[i].readDict(dict);
                        sub.remoteCheckpointRevID = dict["rev"].asString();
                        cLogInfo(i, "Received remote checkpoint (rev='%.*s'): %.*s", SPLAT(sub.remoteCheckpointRevID),
                                 SPLAT(dict.toString()));
                    }
                }

                for ( int i = 0; i < _subRepls.size(); i++ ) {
                    _subRepls[i].remoteCheckpointReceived = true;

                    if ( _subRepls[i].hadLocalCheckpoint ) {
                        // Compare checkpoints, reset if mismatched:
                        bool valid = _subRepls[i].checkpointer->validateWith(remoteCheckpoints[i]);
                        if ( !valid && _subRepls[i].pusher ) _subRepls[i].pusher->checkpointIsInvalid();
                    }
                    // Now we have the checkpoints! Time to start replicating:
                    startReplicating(i);

                    if ( _subRepls[i].checkpointJSONToSave ) saveCheckpointNow(i);
                    // _saveCheckpoint() was waiting for _remoteCheckpointRevID
                }
            }
        });

        _getCollectionsRequested = true;
    }

    void Replicator::_saveCheckpoint(CollectionIndex coll, alloc_slice json) {
        if ( !connected() ) return;
        _subRepls[coll].checkpointJSONToSave = std::move(json);
        if ( _subRepls[coll].remoteCheckpointReceived ) saveCheckpointNow(coll);
        // ...else wait until checkpoint received (see above), which will call saveCheckpointNow().
    }

    void Replicator::saveCheckpointNow(CollectionIndex coll) {
        SubReplicator& sub = _subRepls[coll];
        // Switch to the permanent checkpoint ID:
        alloc_slice checkpointID = sub.checkpointer->checkpointID();
        if ( checkpointID != sub.remoteCheckpointDocID ) {
            sub.remoteCheckpointDocID = checkpointID;
            sub.remoteCheckpointRevID = nullslice;
        }

        alloc_slice json = std::move(sub.checkpointJSONToSave);

        cLogVerbose(coll, "Saving remote checkpoint '%.*s' over rev='%.*s': %.*s ...", SPLAT(sub.remoteCheckpointDocID),
                    SPLAT(sub.remoteCheckpointRevID), SPLAT(json));
        Assert(sub.remoteCheckpointReceived);
        Assert(json);

        MessageBuilder msg("setCheckpoint"_sl);
        assignCollectionToMsg(msg, coll);
        msg["client"_sl] = sub.remoteCheckpointDocID;
        msg["rev"_sl]    = sub.remoteCheckpointRevID;
        msg << json;
        Signpost::begin(Signpost::blipSent);
        sendRequest(msg, [=, &sub](const MessageProgress& progress) {
            if ( progress.state != MessageProgress::kComplete ) return;

            Signpost::end(Signpost::blipSent);
            MessageIn* response = progress.reply;
            if ( response->isError() ) {
                Error responseErr = response->getError();
                if ( responseErr.domain == "HTTP"_sl && responseErr.code == 409 ) {
                    // On conflict, read the remote checkpoint to get the real revID:
                    sub.checkpointJSONToSave      = json;  // move() has no effect here
                    sub.remoteCheckpointRequested = sub.remoteCheckpointReceived = false;
                    getRemoteCheckpoint(true, coll);
                } else {
                    gotError(response);
                    warn("Failed to save remote checkpoint (collection: %u)!", coll);
                    // If the checkpoint didn't save, something's wrong; but if we don't mark it as
                    // saved, the replicator will stay busy (see computeActivityLevel, line 169).
                    sub.checkpointer->saveCompleted();
                }
            } else {
                // Remote checkpoint saved, so update local one:
                sub.remoteCheckpointRevID = response->property("rev"_sl);
                cLogInfo(coll, "Saved remote checkpoint '%.*s' as rev='%.*s'", SPLAT(sub.remoteCheckpointDocID),
                         SPLAT(sub.remoteCheckpointRevID));

                try {
                    _db->useLocked([&](C4Database* db) {
                        _db->markRevsSyncedNow();
                        sub.checkpointer->write(db, json);
                    });
                    cLogInfo(coll, "Saved local checkpoint '%.*s': %.*s", SPLAT(sub.remoteCheckpointDocID),
                             SPLAT(json));
                } catch ( ... ) { gotError(C4Error::fromCurrentException()); }
                sub.checkpointer->saveCompleted();
            }
        });
    }

    bool Replicator::pendingDocumentIDs(C4CollectionSpec spec, Checkpointer::PendingDocCallback callback) {
        // CBL-2448
        auto db = _db;
        if ( !db ) { return false; }

        try {
            bool attempted = false;
            db->useLocked([this, spec, callback, &attempted](const Retained<C4Database>& db) {
                for ( auto& _subRepl : _subRepls ) {
                    if ( _subRepl.collection->getSpec() == spec ) {
                        _subRepl.checkpointer->pendingDocumentIDs(db, callback);
                        attempted = true;
                        break;
                    }
                }
            });
            return attempted;
        } catch ( const error& err ) {
            if ( error{error::Domain::LiteCore, error::LiteCoreError::NotOpen} == err ) {
                return false;
            } else {
                throw;
            }
        }
    }

    optional<bool> Replicator::isDocumentPending(slice docID, C4CollectionSpec spec) {
        // CBL-2448
        auto db = _db;
        if ( !db ) { return nullopt; }

        try {
            return db->useLocked<bool>([this, docID, spec](const Retained<C4Database>& db) {
                for ( auto& _subRepl : _subRepls ) {
                    if ( _subRepl.collection->getSpec() == spec ) {
                        return _subRepl.checkpointer->isDocumentPending(db, docID);
                    }
                }
                throw error(error::LiteCore, error::NotFound,
                            format("collection '%*s' not found", SPLAT(Options::collectionSpecToPath(spec))));
            });
        } catch ( const error& err ) {
            if ( error{error::Domain::LiteCore, error::LiteCoreError::NotOpen} == err ) {
                return nullopt;
            } else {
                throw;
            }
        }
    }

#pragma mark - PEER CHECKPOINT ACCESS:

    // Gets the ID from a checkpoint request
    slice Replicator::getPeerCheckpointDocID(MessageIn* request, const char* whatFor) const {
        slice checkpointID = request->property("client"_sl);
        if ( checkpointID ) logInfo("Request to %s peer checkpoint '%.*s'", whatFor, SPLAT(checkpointID));
        else
            request->respondWithError({"BLIP"_sl, 400, "missing checkpoint ID"_sl});
        return checkpointID;
    }

    // Handles a "getCheckpoint" request by looking up a peer checkpoint.
    void Replicator::handleGetCheckpoint(Retained<MessageIn> request) {
        setMsgHandlerFor3_0_Client(request);

        slice checkpointID = getPeerCheckpointDocID(request, "get");
        if ( !checkpointID ) return;

        auto [checked, err] = checkCollectionOfMsg(*request);
        if ( err ) {
            request->respondWithError({"HTTP"_sl, 400, err});
            return;
        }
        auto collectionIn = checked;

        // We don't save checkpointID by collection.
        (void)collectionIn;

        alloc_slice body, revID;
        int         status = 0;
        try {
            if ( !Checkpointer::getPeerCheckpoint(_db->useLocked(), checkpointID, body, revID) ) status = 404;
        } catch ( ... ) {
            C4Error::warnCurrentException("Replicator::handleGetCheckpoint");
            status = 502;
        }

        if ( status != 0 ) {
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
        setMsgHandlerFor3_0_Client(request);

        slice checkpointID = getPeerCheckpointDocID(request, "set");
        if ( !checkpointID ) return;

        auto [collectionIn, errMsg] = checkCollectionOfMsg(*request);
        if ( errMsg ) {
            request->respondWithError({"BLIP"_sl, 400, errMsg});
            return;
        }
        // We don't save checkpointID by collection
        (void)collectionIn;

        bool        ok;
        alloc_slice newRevID;
        try {
            ok = Checkpointer::savePeerCheckpoint(_db->useLocked(), checkpointID, request->body(),
                                                  request->property("rev"_sl), newRevID);
        } catch ( ... ) {
            request->respondWithError(c4ToBLIPError(C4Error::fromCurrentException()));
            return;
        }

        if ( !ok ) {
            request->respondWithError({"HTTP"_sl, 409, alloc_slice("revision ID mismatch"_sl)});
            return;
        }

        MessageBuilder response(request);
        response["rev"_sl] = newRevID;
        request->respond(response);
    }

    // Handles a "getCollections" request by looking up a peer checkpoint of each collection.
    void Replicator::handleGetCollections(Retained<blip::MessageIn> request) {
        // This message only comes from 3.1+ client.

        if ( !_subRepls.empty() ) {
            // Request of 3.0 style has been received prior to this one.
            logError("Some message has preceded 'getCollections'");
            request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: not the first message"_sl});
            return;
        }

        auto root = request->JSONBody().asDict();
        if ( !root ) {
            request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: no root"_sl});
            return;
        }

        auto checkpointIDs = root["checkpoint_ids"].asArray();
        if ( !checkpointIDs || checkpointIDs.empty() ) {
            request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: no checkpoint_ids"_sl});
            return;
        }

        auto collections = root["collections"].asArray();
        if ( !collections || collections.empty() ) {
            request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: no collections"_sl});
            return;
        }

        if ( checkpointIDs.count() != collections.count() ) {
            request->respondWithError(
                    {"BLIP"_sl, 400, "Invalid getCollections message: mismatched checkpoint_ids and collections"_sl});
            return;
        }

        // Convert to collection specs
        vector<C4CollectionSpec> collSpecs;
        collSpecs.reserve(collections.count());
        for ( fleece::Array::iterator i(collections); i; ++i ) {
            if ( !i.value().asString() ) {
                request->respondWithError({"BLIP"_sl, 400, "Invalid getCollections message: empty collection path"_sl});
                return;
            }
            collSpecs.emplace_back(Options::collectionPathToSpec(i.value().asString()));
        }

        // check duplicates
        std::unordered_set<C4CollectionSpec> specSet;
        for ( auto& collSpec : collSpecs ) {
            bool b;
            std::tie(std::ignore, b) = specSet.insert(collSpec);
            if ( !b ) {
                request->respondWithError(
                        {"BLIP"_sl, 400, "Invalid getCollections message: duplicate collection path"_sl});
                return;
            }
        }

        // Create options->workingCollections according to collSpecs. If i-th CollectionSpec
        // is not found, put an empty collectionOptions at i-th position.
        _options->rearrangeCollections(collSpecs);
        MessageBuilder response(request);
        auto&          enc = response.jsonBody();
        enc.beginArray();

        bool hasUnfoundCollection = false;
        for ( int i = 0; i < checkpointIDs.count(); i++ ) {
            auto checkpointID = checkpointIDs[i].asString();

            cLogInfo(i, "Request to get peer checkpoint '%.*s' for collection", SPLAT(checkpointID));

            if ( !_options->collectionPath(i) ) {
                cLogVerbose(
                        i, "Get peer checkpoint '%.*s' for collection: Collection Not Found in the Replicator's config",
                        SPLAT(checkpointID));
                enc.writeNull();
                hasUnfoundCollection = true;
                continue;
            }

            alloc_slice body, revID;
            int         status = 0;
            try {
                if ( !Checkpointer::getPeerCheckpoint(_db->useLocked(), checkpointID, body, revID) ) {
                    enc.writeValue(Dict::emptyDict());
                    continue;
                }
            } catch ( ... ) {
                C4Error::warnCurrentException("Replicator::handleGetCollections");
                status = 502;
            }

            if ( status != 0 ) {
                request->respondWithError({"HTTP"_sl, status});
                return;
            }

            FLError flError = kFLNoError;
            Doc     doc     = Doc::fromJSON(body, &flError);
            DebugAssert(flError == kFLNoError);
            auto checkpoint = doc.root().asDict().mutableCopy();
            checkpoint.set("rev"_sl, revID);
            enc.writeValue(checkpoint);
        }
        enc.endArray();

        if ( !hasUnfoundCollection ) {
            prepareWorkers();
            DebugAssert(_options->workingCollectionCount() == _subRepls.size()
                        && _options->workingCollectionCount() == collSpecs.size());
        }
        // Else, with current plan, the client should not proceed normally.
        // It should move to stop

        request->respond(response);
    }

    void Replicator::prepareWorkers() {
        _subRepls.resize(_options->workingCollectionCount());

        // Retained C4Collection object may become invalid if the underlying collection
        // is deleted. By spec, all collections must exist when replication starts,
        // and it is an error if any collection is deleted while in progress.
        // Note: retained C4Collection* may blow up if it is used after becoming invalid,
        // and this is expected.
        _db->useLocked([this](Retained<C4Database>& db) {
            for ( CollectionIndex i = 0; i < _options->workingCollectionCount(); ++i ) {
                C4Collection* c = db->getCollection(_options->collectionSpec(i));
                if ( c == nullptr ) {
                    _subRepls.clear();
                    error::_throw(error::NotFound, "collection %s is not found in the database.",
                                  _options->collectionPath(i).asString().c_str());
                }
                _subRepls[i].collection = c;
            }
        });

        actor::Timer::duration saveDelay = tuning::kDefaultCheckpointSaveDelay;
        if ( auto i = _options->properties[kC4ReplicatorCheckpointInterval].asInt(); i > 0 )
            saveDelay = chrono::seconds(i);

        bool isPushBusy = false;
        bool isPullBusy = false;
        for ( CollectionIndex i = 0; i < _options->workingCollectionCount(); ++i ) {
            SubReplicator& sub = _subRepls[i];
            if ( _options->push(i) != kC4Disabled ) {
                sub.checkpointer = std::make_unique<Checkpointer>(_options, _remoteURL, sub.collection);
                sub.pusher       = new Pusher(this, *sub.checkpointer, i);
                sub.pushStatus   = Worker::Status(kC4Busy);
                isPushBusy       = true;
            } else {
                sub.pushStatus = Worker::Status(kC4Stopped);
            }
            if ( _options->pull(i) != kC4Disabled ) {
                sub.puller     = new Puller(this, i);
                sub.pullStatus = Worker::Status(kC4Busy);
                if ( sub.checkpointer == nullptr ) {
                    sub.checkpointer = std::make_unique<Checkpointer>(_options, _remoteURL, sub.collection);
                }
                isPullBusy = true;
            } else {
                sub.pullStatus = Worker::Status(kC4Stopped);
            }
            DebugAssert(sub.checkpointer.get() != nullptr);
            sub.checkpointer->enableAutosave(
                    saveDelay, [this, i](auto&& PH1) { saveCheckpoint(i, std::forward<decltype(PH1)>(PH1)); });
        }
        _pushStatus = Worker::Status(isPushBusy ? kC4Busy : kC4Stopped);
        _pullStatus = Worker::Status(isPullBusy ? kC4Busy : kC4Stopped);

        // Get the remote DB ID:
        slice key;
        // Assertion: _collections.size() > 0
        // All _checkpointer's share the same key.
        key                   = _subRepls[0].checkpointer->remoteDBIDString();
        C4RemoteID remoteDBID = _db->lookUpRemoteDBID(key);
        logVerbose("Remote-DB ID %u found for target <%.*s>", remoteDBID, SPLAT(key));
    }

    void Replicator::delegateCollectionSpecificMessageToWorker(Retained<blip::MessageIn> request) {
        setMsgHandlerFor3_0_Client(request);

        slice profile = request->property("Profile"_sl);
        Assert(profile);

        auto [i, errMsg] = checkCollectionOfMsg(*request);
        if ( errMsg ) {
            if ( errMsg ) {
                request->respondWithError({"HTTP"_sl, 400, errMsg});
                return;
            }
        }

#ifdef LITECORE_CPPTEST
        if ( _delayChangesResponse && (profile == "changes"_sl || profile == "proposeChanges"_sl) ) {
            C4Log("Delaying changes response...");
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
#endif

        auto it = _workerHandlers.find({profile.asString(), i});
        if ( it != _workerHandlers.end() ) {
            it->second(request);
        } else {
            returnForbidden(request);
        }
    }

    // This method is to properly initialize the passive replicator to get ready to
    // serve 3.0 replicator, which is unaware of the collection.
    // It is a no-op if
    // 1. it is working in the active mode,                             or
    // 2. the incoming meesage includes explicit "collection" property, or
    // 3. the second time and after that this method is called.
    void Replicator::setMsgHandlerFor3_0_Client(const Retained<blip::MessageIn>& request) {
        if ( _setMsgHandlerFor3_0_ClientDone ) {
            return;
        } else {
            _setMsgHandlerFor3_0_ClientDone = true;
        }

        if ( _options->isActive() ) {
            return;  // only deal with passive replicator.
        }

        if ( request->intProperty(kCollectionProperty, kNotCollectionIndex) != kNotCollectionIndex ) {
            return;  // 3.0 message should not include the collection property
        }

        // At this point, we are dealing with a 3.0 style replicator which can only have exactly
        // one collection, which is the default one.  If the default collection is not specified in
        // the passive config, rearrangeCollectionsFor3_0_Client() will put a null collection path
        // at the place of index 0, and then we return an error here.
        // (If the collection does not exist in the database, prepareWorkers() will fail and an
        //  error will be returned from there.)

        _options->rearrangeCollectionsFor3_0_Client();
        DebugAssert(!_options->collectionAware());
        DebugAssert(_options->workingCollectionCount() == 1);
        if ( !_options->collectionPath(0) ) {
            logVerbose("Client is legacy 3.0, but the default collection is not in the config of this 3.1 replicator.");
            request->respondWithError({"BLIP"_sl, 400, "This server is not configured for 3.0 client support"_sl});
            return;
        } else {
            prepareWorkers();
        }
    }
}  // namespace litecore::repl
