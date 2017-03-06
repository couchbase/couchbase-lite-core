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
                           Options options,
                           Connection *connection)
    :ReplActor(connection, options, "Repl")
    ,_remoteAddress(address)
    ,_pushing(options.push > kC4Passive)
    ,_pulling(options.pull > kC4Passive)
    ,_dbActor(new DBActor(connection, db, address, options))
    {
        if (options.push != kC4Disabled)
            _pusher = new Pusher(connection, this, _dbActor, _options);
        if (options.pull != kC4Disabled)
            _puller = new Puller(connection, this, _dbActor, _options);
        _checkpoint.enableAutosave(options.checkpointSaveDelay,
                                  asynchronize([this](alloc_slice json){ saveCheckpoint(json); }));
        // Now wait for _onConnect or _onClose...
    }

    Replicator::Replicator(C4Database *db,
                           websocket::Provider &provider,
                           const websocket::Address &address,
                           Options options)
    :Replicator(db, address, options, new Connection(address, provider, *this))
    { }

    Replicator::Replicator(C4Database *db,
                           websocket::WebSocket *webSocket,
                           const websocket::Address& address,
                           Options options)
    :Replicator(db, address, options, new Connection(webSocket, *this))
    { }


    // Called after the checkpoint is established.
    void Replicator::startReplicating() {
        auto cp = _checkpoint.sequences();
        if (_options.push > kC4Passive)
            _pusher->start(cp.local);
        if (_options.pull > kC4Passive)
            _puller->start(cp.remote);
    }


    // The Pusher or Puller has finished.
    void Replicator::_taskComplete(bool isPush) {
        if (isPush)
            _pushing = false;
        else
            _pulling = false;
        _checkpoint.save();
    }


    void Replicator::afterEvent() {
        // Decide whether a non-continuous active push or pull replication is done:
        if (!_pushing && !_pulling && !isBusy() && isOpenClient()) {
            log("Replication complete! Closing connection");
            connection()->close();
        }
    }


#pragma mark - BLIP DELEGATE:


    void Replicator::_onConnect() {
        log("BLIP Connected");
        if (_options.push > kC4Passive || _options.pull > kC4Passive)
            getCheckpoints();
    }


    void Replicator::_onClose(bool normalClose, int status, alloc_slice reason) {
        if (normalClose)
            log("Connection closed: %.*s (status %d)", SPLAT(reason), status);
        else
            logError("Disconnected: %.*s (error %d)", SPLAT(reason), status);

        _checkpoint.stopAutosave();

        // Clear connection() and notify the other agents to do the same:
        _connectionClosed();
        _dbActor->connectionClosed();
        if (_pusher)
            _pusher->connectionClosed();
        if (_puller)
            _puller->connectionClosed();
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
            sendRequest(msg, [this](MessageIn *response) {
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


    void Replicator::saveCheckpoint(alloc_slice json) {
        log("Saving remote checkpoint %.*s with rev='%.*s' ...",
            SPLAT(_checkpointDocID), SPLAT(_checkpointRevID));
        MessageBuilder msg("setCheckpoint"_sl);
        msg["client"_sl] = _checkpointDocID;
        msg["rev"_sl] = _checkpointRevID;
        msg << json;
        sendRequest(msg, [=](MessageIn *response) {
            if (response->isError()) {
                gotError(response);
                // TODO: On 409 error, reload remote checkpoint
            } else {
                // Remote checkpoint saved, so update local one:
                _checkpointRevID = response->property("rev"_sl);
                log("Successfully saved remote checkpoint %.*s as rev='%.*s'",
                    SPLAT(_checkpointDocID), SPLAT(_checkpointRevID));
                _dbActor->setCheckpoint(json);
            }
            // Tell the checkpoint the save is finished, so it will call me again
            _checkpoint.saved();
        });
    }

} }
