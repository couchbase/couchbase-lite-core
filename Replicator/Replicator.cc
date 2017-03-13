//
//  Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

//  https://github.com/couchbaselabs/couchbase-lite-api/wiki/New-Replication-Protocol

#include "Replicator.hh"
#include "DBActor.hh"
#include "Pusher.hh"
#include "Puller.hh"
#include "StringUtil.hh"
#include "Logging.hh"
#include "SecureDigest.hh"

using namespace std;
using namespace std::placeholders;
using namespace fleece;
using namespace fleeceapi;


namespace litecore { namespace repl {


    Replicator::Replicator(C4Database* db,
                           const websocket::Address &address,
                           Delegate &delegate,
                           Options options,
                           Connection *connection)
    :ReplActor(connection, options, "Repl")
    ,_remoteAddress(address)
    ,_delegate(delegate)
    ,_pushActivity(options.push == kC4Disabled ? kC4Stopped : kC4Busy)
    ,_pullActivity(options.pull == kC4Disabled ? kC4Stopped : kC4Busy)
    ,_dbActor(new DBActor(connection, db, address, options))
    {
        if (options.push != kC4Disabled)
            _pusher = new Pusher(connection, this, _dbActor, _options);
        if (options.pull != kC4Disabled)
            _puller = new Puller(connection, this, _dbActor, _options);
        _checkpoint.enableAutosave(options.checkpointSaveDelay,
                                   bind(&Replicator::saveCheckpoint, this, _1));
        // Now wait for _onConnect or _onClose...
    }

    Replicator::Replicator(C4Database *db,
                           websocket::Provider &provider,
                           const websocket::Address &address,
                           Delegate &delegate,
                           Options options)
    :Replicator(db, address, delegate, options, new Connection(address, provider, *this))
    { }

    Replicator::Replicator(C4Database *db,
                           websocket::WebSocket *webSocket,
                           const websocket::Address& address,
                           Delegate &delegate,
                           Options options)
    :Replicator(db, address, delegate, options, new Connection(webSocket, *this))
    { }


    void Replicator::_stop() {
        if (connection())
            connection()->close();
    }


    // Called after the checkpoint is established.
    void Replicator::startReplicating() {
        auto cp = _checkpoint.sequences();
        if (_options.push > kC4Passive)
            _pusher->start(cp.local);
        if (_options.pull > kC4Passive)
            _puller->start(cp.remote);
    }


    // The Pusher or Puller has finished.
    void Replicator::_taskChangedActivityLevel(ReplActor *task, ActivityLevel level) {
        if (task == _pusher)
            _pushActivity = level;
        else if (task == _puller)
            _pullActivity = level;

        logDebug("pushActivity=%d, pullActivity=%d", _pushActivity, _pullActivity);
        if (level == kC4Stopped)
            _checkpoint.save();
    }


    ReplActor::ActivityLevel Replicator::computeActivityLevel() const {
        if (!connection())
            return kC4Stopped;
        switch (connection()->state()) {
            case Connection::kDisconnected:
            case Connection::kClosed:
            case Connection::kClosing:
                return kC4Stopped;
            case Connection::kConnecting:
                return kC4Connecting;
            case Connection::kConnected: {
                ActivityLevel level;
                if (_checkpoint.isUnsaved())
                    level = kC4Busy;
                else
                    level = ReplActor::computeActivityLevel();
                if (level == kC4Idle && !isOpenServer())
                    level = kC4Stopped;
                return max(level, max(_pushActivity, _pullActivity));
            }
        }
    }


    void Replicator::activityLevelChanged(ActivityLevel level) {
        // Decide whether a non-continuous active push or pull replication is done:
        if (level == kC4Stopped && connection()) {
            log("Replication complete! Closing connection");
            connection()->close();
        }
        _delegate.replicatorActivityChanged(this, level);
    }


#pragma mark - BLIP DELEGATE:


    void Replicator::_onConnect() {
        log("BLIP Connected");
        if (_options.push > kC4Passive || _options.pull > kC4Passive)
            getCheckpoints();
    }


    void Replicator::_onClose(Connection::CloseStatus status) {
        static const char* kReasonNames[] = {"WebSocket status", "errno", "DNS error"};
        log("Connection closed with %s %d: %.*s",
            kReasonNames[status.reason], status.code, SPLAT(status.message));

        _checkpoint.stopAutosave();

        //TODO: Save the error info
        // Clear connection() and notify the other agents to do the same:
        _connectionClosed();
        _dbActor->connectionClosed();
        if (_pusher)
            _pusher->connectionClosed();
        if (_puller)
            _puller->connectionClosed();

        _closeStatus = status;
        _delegate.replicatorConnectionClosed(this, status);
    }


    // This only gets called if none of the registered handlers were triggered.
    void Replicator::_onRequestReceived(Retained<MessageIn> msg) {
        warn("Received unrecognized BLIP request #%llu with Profile '%.*s', %zu bytes",
                msg->number(), SPLAT(msg->property("Profile"_sl)), msg->body().size);
        msg->respondWithError("BLIP"_sl, 404);
    }


#pragma mark - CHECKPOINT:


    // Start off by getting the local & remote checkpoints, if this is an active replicator:
    void Replicator::getCheckpoints() {
        // Get the local checkpoint:
        _dbActor->getCheckpoint(asynchronize([this](alloc_slice checkpointID,
                                                    alloc_slice data,
                                                    C4Error err) {
            _checkpointDocID = checkpointID;

            if (!data) {
                if (err.code) {
                    gotError(err);
                } else {
                    // Skip getting remote checkpoint since there's no local one.
                    log("No local checkpoint '%.*s'", SPLAT(checkpointID));
                    startReplicating();
                }
                return;
            }

            _checkpoint.decodeFrom(data);
            auto cp = _checkpoint.sequences();
            log("Local checkpoint '%.*s' is [%llu, '%.*s']; getting remote ...",
                  SPLAT(checkpointID), cp.local, SPLAT(cp.remote));

            // Get the remote checkpoint, using the same checkpointID:
            MessageBuilder msg("getCheckpoint"_sl);
            msg["client"_sl] = checkpointID;
            sendRequest(msg, [this](MessageProgress progress) {
                MessageIn *response = progress.reply;
                if (!response)
                    return;
                Checkpoint remoteCheckpoint;

                if (response->isError()) {
                    if (!(response->errorDomain() == "HTTP"_sl && response->errorCode() == 404))
                        return gotError(response);
                    log("No remote checkpoint");
                    _checkpointRevID.reset();
                } else {
                    log("Received remote checkpoint: '%.*s'", SPLAT(response->body()));
                    remoteCheckpoint.decodeFrom(response->body());
                    _checkpointRevID = response->property("rev"_sl);
                }

                auto gotcp = remoteCheckpoint.sequences();
                log("...got remote checkpoint: [%llu, '%.*s'] rev='%.*s'",
                    gotcp.local, SPLAT(gotcp.remote), SPLAT(_checkpointRevID));

                // Reset mismatched checkpoints:
                _checkpoint.validateWith(remoteCheckpoint);

                // Now we have the checkpoints! Time to start replicating:
                startReplicating();
            });
        }));
    }


    void Replicator::_saveCheckpoint(alloc_slice json) {
        log("Saving remote checkpoint %.*s with rev='%.*s' ...",
            SPLAT(_checkpointDocID), SPLAT(_checkpointRevID));
        MessageBuilder msg("setCheckpoint"_sl);
        msg["client"_sl] = _checkpointDocID;
        msg["rev"_sl] = _checkpointRevID;
        msg << json;
        sendRequest(msg, [=](MessageProgress progress) {
            MessageIn *response = progress.reply;
            if (!response)
                return;
            if (response->isError()) {
                gotError(response);
                // TODO: On 409 error, reload remote checkpoint
            } else {
                // Remote checkpoint saved, so update local one:
                _checkpointRevID = response->property("rev"_sl);
                log("Successfully saved remote checkpoint %.*s as rev='%.*s'",
                    SPLAT(_checkpointDocID), SPLAT(_checkpointRevID));
                _dbActor->setCheckpoint(json, asynchronize([this]{
                    _checkpoint.saved();
                }));
            }
            // Tell the checkpoint the save is finished, so it will call me again
        });
    }

} }
