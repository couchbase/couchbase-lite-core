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
    :ReplActor(connection, options, connection->name())
    ,_remoteAddress(address)
    ,_pushing(options.push)
    ,_pulling(options.pull)
    ,_dbActor(new DBActor(connection, db, address, options))
    ,_pusher(new Pusher(connection, this, _dbActor, _options))
    ,_puller(new Puller(connection, this, _dbActor, _options))
    {
        _checkpoint.autosave(chrono::seconds(5),
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
                           const websocket::Address& address)
    :Replicator(db, address, Options{}, new Connection(webSocket, *this))
    { }


    // Called after the checkpoint is established.
    void Replicator::startReplicating() {
        if (_options.push)
            _pusher->start(_checkpoint.localSeq());
        if (_options.pull)
            _puller->start(_checkpoint.remoteSeq());
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
        if (!isBusy() && !_pushing && !_pulling && !_options.continuous
                    && connection() && !connection()->isServer()) {
            log("Replication complete! Closing connection");
            connection()->close();
        }
    }


#pragma mark - BLIP DELEGATE:


    void Replicator::_onConnect() {
        log("BLIP Connected");
        if (_options.push || _options.pull)
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
        _pusher->connectionClosed();
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
            log("Local checkpoint '%.*s' is [%llu, '%.*s']; getting remote ...",
                  SPLAT(checkpointID), _checkpoint.localSeq(), SPLAT(_checkpoint.remoteSeq()));

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

                log("...got remote checkpoint: [%llu, '%.*s'] rev='%.*s'",
                    remoteCheckpoint.localSeq(), SPLAT(remoteCheckpoint.remoteSeq()),
                    SPLAT(_checkpointRevID));

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
        });
    }

} }
