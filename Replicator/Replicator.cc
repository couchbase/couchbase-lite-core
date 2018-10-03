//
// Replicator.cc
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

#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "DBWorker.hh"
#include "Pusher.hh"
#include "Puller.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Logging.hh"
#include "SecureDigest.hh"
#include "BLIP.hh"
#include "Address.hh"
#include "Instrumentation.hh"

using namespace std;
using namespace std::placeholders;
using namespace fleece;


namespace litecore { namespace repl {

    Replicator::Replicator(C4Database* db,
                           websocket::WebSocket *webSocket,
                           Delegate &delegate,
                           Options options)
    :Worker(new Connection(webSocket, options.properties, *this),
            nullptr, options, "Repl")
    ,_delegate(&delegate)
    ,_connectionState(connection()->state())
    ,_pushStatus(options.push == kC4Disabled ? kC4Stopped : kC4Busy)
    ,_pullStatus(options.pull == kC4Disabled ? kC4Stopped : kC4Busy)
    ,_dbActor(new DBWorker(this, db, webSocket->url()))
    {
        _loggingID = string(alloc_slice(c4db_getPath(db))) + " " + _loggingID;
        _important = 2;

        log("%s", string(options).c_str());

        if (options.push != kC4Disabled)
            _pusher = new Pusher(this, _dbActor);
        if (options.pull != kC4Disabled)
            _puller = new Puller(this, _dbActor);
        _checkpoint.enableAutosave(options.checkpointSaveDelay(),
                                   bind(&Replicator::saveCheckpoint, this, _1));
    }


    void Replicator::start(bool synchronous) {
        if (synchronous)
            _start();
        else
            enqueue(&Replicator::_start);
    }

    void Replicator::_start() {
        Assert(_connectionState == Connection::kClosed);
        Signpost::mark(Signpost::replicatorStart, uint32_t(size_t(this)));
        _connectionState = Connection::kConnecting;
        connection()->start();
        // Now wait for _onConnect or _onClose...

        if (_options.push > kC4Passive || _options.pull > kC4Passive)
            getLocalCheckpoint();
    }


    void Replicator::_stop() {
        log("Told to stop!");
        _disconnect(websocket::kCodeNormal, {});
    }

    void Replicator::_disconnect(websocket::CloseCode closeCode, slice message) {
        auto conn = connection();
        if (conn) {
            conn->close(closeCode, message);
            _connectionState = Connection::kClosing;
        }
    }


    // Called after the checkpoint is established.
    void Replicator::startReplicating() {
        auto cp = _checkpoint.sequences();
        if (_options.push > kC4Passive)
            _pusher->start(cp.local);
        if (_options.pull > kC4Passive)
            _puller->start(cp.remote);
    }


#pragma mark - STATUS:


    // The status of one of the actors has changed; update mine
    void Replicator::_childChangedStatus(Worker *task, Status taskStatus)
    {
        if (status().level == kC4Stopped)       // I've already stopped & cleared refs; ignore this
            return;

        if (task == _pusher) {
            _pushStatus = taskStatus;
        } else if (task == _puller) {
            _pullStatus = taskStatus;
        } else if (task == _dbActor) {
            _dbStatus = taskStatus;
        }

        setProgress(_pushStatus.progress + _pullStatus.progress);

        if (SyncBusyLog.effectiveLevel() <= LogLevel::Info) {
            log("pushStatus=%-s, pullStatus=%-s, dbStatus=%-s, progress=%llu/%llu",
                kC4ReplicatorActivityLevelNames[_pushStatus.level],
                kC4ReplicatorActivityLevelNames[_pullStatus.level],
                kC4ReplicatorActivityLevelNames[_dbStatus.level],
                status().progress.unitsCompleted, status().progress.unitsTotal);
        }

        if (_pullStatus.error.code)
            onError(_pullStatus.error);
        else if (_pushStatus.error.code)
            onError(_pushStatus.error);

        // Save a checkpoint immediately when push or pull finishes or goes idle:
        if ((taskStatus.level == kC4Stopped || taskStatus.level == kC4Idle)
                && (task == _pusher || task == _puller))
            _checkpoint.save();
    }


    Worker::ActivityLevel Replicator::computeActivityLevel() const {
        ActivityLevel level;
        switch (_connectionState) {
            case Connection::kConnecting:
                level = kC4Connecting;
                break;
            case Connection::kConnected: {
                if (_checkpoint.isUnsaved())
                    level = kC4Busy;
                else
                    level = Worker::computeActivityLevel();
                level = max(level, max(_pushStatus.level, _pullStatus.level));
                if (level == kC4Idle && !isContinuous() && !isOpenServer()) {
                    // Detect that a non-continuous active push or pull replication is done:
                    log("Replication complete! Closing connection");
                    const_cast<Replicator*>(this)->_stop();
                    level = kC4Busy;
                }
                DebugAssert(level > kC4Stopped);
                break;
            }
            case Connection::kClosing:
                // Remain active while I wait for the connection to finish closing:
                level = kC4Busy;
                break;
            case Connection::kDisconnected:
            case Connection::kClosed:
                // After connection closes, remain active while I wait for db to finish writes:
                level = (_dbStatus.level == kC4Busy) ? kC4Busy : kC4Stopped;
                break;
        }
        if (SyncBusyLog.effectiveLevel() <= LogLevel::Info) {
            log("activityLevel=%-s: connectionState=%d",
                kC4ReplicatorActivityLevelNames[level], _connectionState);
        }
        return level;
    }


    void Replicator::onError(C4Error error) {
        Worker::onError(error);
        if (error.domain == LiteCoreDomain && error.code == kC4ErrorUnexpectedError) {
            // Treat an exception as a fatal error for replication:
            alloc_slice message( c4error_getMessage(error) );
            logError("Stopping due to fatal error: %.*s", SPLAT(message));
            _disconnect(websocket::kCodeUnexpectedCondition, "An exception was thrown"_sl);
        }
    }


    void Replicator::changedStatus() {
        if (status().level == kC4Stopped) {
            DebugAssert(!connection());  // must already have gotten _onClose() delegate callback
            _pusher = nullptr;
            _puller = nullptr;
            _dbActor = nullptr;
        }
        if (_delegate) {
            // Notify the delegate of the current status, but not too often:
            auto waitFor = tuning::kMinDelegateCallInterval - _sinceDelegateCall.elapsed();
            if (waitFor <= 0 || status().level != _lastDelegateCallLevel) {
                reportStatus();
            } else if (!_waitingToCallDelegate) {
                _waitingToCallDelegate = true;
                enqueueAfter(actor::delay_t(waitFor), &Replicator::reportStatus);
            }
        }
    }


    void Replicator::reportStatus() {
        _waitingToCallDelegate = false;
        _lastDelegateCallLevel = status().level;
        _sinceDelegateCall.reset();
        if (_delegate)
            _delegate->replicatorStatusChanged(this, status());
        if (status().level == kC4Stopped)
            _delegate = nullptr;        // Never call delegate after telling it I've stopped
    }


    void Replicator::_endedDocument(alloc_slice docID, Dir dir, C4Error error, bool transient) {
        if (_delegate)
            _delegate->replicatorDocumentEnded(this, dir, docID, error, transient);
    }


    void Replicator::_onBlobProgress(BlobProgress p) {
        if (_delegate)
            _delegate->replicatorBlobProgress(this, p);
    }


#pragma mark - BLIP DELEGATE:


    void Replicator::_onHTTPResponse(int status, fleece::AllocedDict headers) {
        if (status == 101 && !headers["Sec-WebSocket-Protocol"]) {
            gotError(c4error_make(WebSocketDomain, kWebSocketCloseProtocolError,
                                  "Incompatible replication protocol "
                                  "(missing 'Sec-WebSocket-Protocol' response header)"_sl));
        }
        auto setCookie = headers["Set-Cookie"_sl];
        if (setCookie.type() == kFLArray) {
            // Yes, there can be multiple Set-Cookie headers.
            for (Array::iterator i(setCookie.asArray()); i; ++i) {
                _dbActor->setCookie(i.value().asString());
            }
        } else if (setCookie) {
            _dbActor->setCookie(setCookie.asString());
        }
        if (_delegate)
            _delegate->replicatorGotHTTPResponse(this, status, headers);
    }


    void Replicator::_onConnect() {
        log("Connected!");
        Signpost::mark(Signpost::replicatorConnect, uint32_t(size_t(this)));
        if (_connectionState != Connection::kClosing) {     // skip this if stop() already called
            _connectionState = Connection::kConnected;
            if (_options.push > kC4Passive || _options.pull > kC4Passive)
                getRemoteCheckpoint();
        }
    }


    void Replicator::_onClose(Connection::CloseStatus status, Connection::State state) {
        log("Connection closed with %-s %d: \"%.*s\" (state=%d)",
            status.reasonName(), status.code, SPLAT(status.message), _connectionState);
        Signpost::mark(Signpost::replicatorDisconnect, uint32_t(size_t(this)));

        bool closedByPeer = (_connectionState != Connection::kClosing);
        _connectionState = state;

        _checkpoint.stopAutosave();

        // Clear connection() and notify the other agents to do the same:
        _connectionClosed();
        _dbActor->connectionClosed();
        if (_pusher)
            _pusher->connectionClosed();
        if (_puller)
            _puller->connectionClosed();

        if (status.isNormal() && closedByPeer && (_options.push > kC4Passive || _options.pull > kC4Passive)) {
            log("I didn't initiate the close; treating this as code 1001 (GoingAway)");
            status.code = websocket::kCodeGoingAway;
            status.message = alloc_slice("WebSocket connection closed by peer");
        }
        _closeStatus = status;

        static const C4ErrorDomain kDomainForReason[] = {WebSocketDomain, POSIXDomain,
                                                         NetworkDomain, LiteCoreDomain};

        // If this was an unclean close, set my error property:
        if (status.reason != websocket::kWebSocketClose || status.code != websocket::kCodeNormal) {
            int code = status.code;
            C4ErrorDomain domain;
            if (status.reason < sizeof(kDomainForReason)/sizeof(C4ErrorDomain))
                domain = kDomainForReason[status.reason];
            else {
                domain = LiteCoreDomain;
                code = kC4ErrorRemoteError;
            }
            gotError(c4error_make(domain, code, status.message));
        }

        if (_delegate)
            _delegate->replicatorConnectionClosed(this, status);
    }


    // This only gets called if none of the registered handlers were triggered.
    void Replicator::_onRequestReceived(Retained<MessageIn> msg) {
        warn("Received unrecognized BLIP request #%llu with Profile '%.*s', %zu bytes",
                msg->number(), SPLAT(msg->property("Profile"_sl)), msg->body().size);
        msg->notHandled();
    }


#pragma mark - CHECKPOINT:


    // Start off by getting the local checkpoint, if this is an active replicator:
    void Replicator::getLocalCheckpoint() {
        _dbActor->getCheckpoint(asynchronize([this](alloc_slice checkpointID,
                                                    alloc_slice data,
                                                    bool dbIsEmpty,
                                                    C4Error err) {
            // ...after the checkpoint is read:
            if (status().level == kC4Stopped)
                return;
            
            _checkpointDocID = checkpointID;

            if (_options.properties[kC4ReplicatorResetCheckpoint].asBool()) {
                log("Ignoring local checkpoint ('reset' option is set)");
            } else if (data) {
                _checkpoint.decodeFrom(data);
                auto cp = _checkpoint.sequences();
                log("Local checkpoint '%.*s' is [%llu, '%.*s']; getting remote ...",
                    SPLAT(checkpointID), cp.local, SPLAT(cp.remote));
                _hadLocalCheckpoint = true;
            } else if (err.code == 0) {
                log("No local checkpoint '%.*s'", SPLAT(checkpointID));
                // If pulling into an empty db with no checkpoint, it's safe to skip deleted
                // revisions as an optimization.
                if (dbIsEmpty && _options.pull > kC4Passive)
                    _puller->setSkipDeleted();
            } else {
                log("Fatal error getting local checkpoint");
                gotError(err);
                stop();
                return;
            }
            
            getRemoteCheckpoint();
        }));
    }


    // Get the remote checkpoint, after we've got the local one and the BLIP connection is up.
    void Replicator::getRemoteCheckpoint() {
        // Get the remote checkpoint, using the same checkpointID:
        if (!_checkpointDocID || _connectionState != Connection::kConnected)
            return;     // not ready yet
        if (_remoteCheckpointRequested)
            return;     // already in progress

        logVerbose("Requesting remote checkpoint");
        MessageBuilder msg("getCheckpoint"_sl);
        msg["client"_sl] = _checkpointDocID;
        sendRequest(msg, [this](MessageProgress progress) {
            // ...after the checkpoint is received:
            if (progress.state != MessageProgress::kComplete)
                return;
            MessageIn *response = progress.reply;
            Checkpoint remoteCheckpoint;

            if (response->isError()) {
                auto err = response->getError();
                if (!(err.domain == "HTTP"_sl && err.code == 404))
                    return gotError(response);
                log("No remote checkpoint");
                _checkpointRevID.reset();
            } else {
                remoteCheckpoint.decodeFrom(response->body());
                _checkpointRevID = response->property("rev"_sl);
                if (willLog()) {
                    auto gotcp = remoteCheckpoint.sequences();
                    log("Received remote checkpoint: [%llu, '%.*s'] rev='%.*s'",
                        gotcp.local, SPLAT(gotcp.remote), SPLAT(_checkpointRevID));
                }
            }
            _remoteCheckpointReceived = true;

            if (_hadLocalCheckpoint) {
                // Compare checkpoints, reset if mismatched:
                bool valid = _checkpoint.validateWith(remoteCheckpoint);
                if (!valid)
                    _dbActor->checkpointIsInvalid();

                // Now we have the checkpoints! Time to start replicating:
                startReplicating();
            }

            if (_checkpointJSONToSave)
                saveCheckpointNow();    // _saveCheckpoint() was waiting for _checkpointRevID
        });

        _remoteCheckpointRequested = true;

        // If there's no local checkpoint, we know we're starting from zero and don't need to
        // wait for the remote one before getting started:
        if (!_hadLocalCheckpoint)
            startReplicating();
    }


    void Replicator::_saveCheckpoint(alloc_slice json) {
        if (!connection())
            return;
        _checkpointJSONToSave = move(json);
        if (_remoteCheckpointReceived)
            saveCheckpointNow();
        // ...else wait until checkpoint received (see above), which will call saveCheckpointNow().
    }


    void Replicator::saveCheckpointNow() {
        alloc_slice json = move(_checkpointJSONToSave);

        logVerbose("Saving remote checkpoint %.*s with rev='%.*s': %.*s ...",
                   SPLAT(_checkpointDocID), SPLAT(_checkpointRevID), SPLAT(json));
        Assert(_remoteCheckpointReceived);
        Assert(json);

        MessageBuilder msg("setCheckpoint"_sl);
        msg["client"_sl] = _checkpointDocID;
        msg["rev"_sl] = _checkpointRevID;
        msg << json;
        sendRequest(msg, [=](MessageProgress progress) {
            if (progress.state != MessageProgress::kComplete)
                return;
            MessageIn *response = progress.reply;
            if (response->isError()) {
                gotError(response);
                warn("Failed to save checkpoint!");
                // If the checkpoint didn't save, something's wrong; but if we don't mark it as
                // saved, the replicator will stay busy (see computeActivityLevel, line 169).
                _checkpoint.saved();
                // TODO: On 409 error, reload remote checkpoint
            } else {
                // Remote checkpoint saved, so update local one:
                _checkpointRevID = response->property("rev"_sl);
                log("Saved remote checkpoint %.*s as rev='%.*s'",
                    SPLAT(_checkpointDocID), SPLAT(_checkpointRevID));
                _dbActor->setCheckpoint(json, asynchronize([this]{
                    _checkpoint.saved();
                }));
            }
        });
    }

} }
