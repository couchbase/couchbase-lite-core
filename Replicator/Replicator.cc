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
            _pusher->start(_checkpoint.localSeq);
        if (_options.pull)
            _puller->start(_checkpoint.remoteSeq);
    }


    // The Pusher or Puller has finished.
    void Replicator::_taskComplete(bool isPush) {
        if (isPush)
            _pushing = false;
        else
            _pulling = false;
        if (_checkpointChanged)
            _updateCheckpoint();
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
                                                    alloc_slice rev,
                                                    C4Error err) {
            _checkpointID = checkpointID;

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

            _checkpoint = decodeCheckpoint(data);
            log("Local checkpoint '%.*s' is [%llu, '%s']; getting remote ...",
                  SPLAT(checkpointID), _checkpoint.localSeq, _checkpoint.remoteSeq.c_str());

            MessageBuilder msg("getCheckpoint"_sl);
            msg["client"_sl] = checkpointID;
            sendRequest(msg, [this](MessageIn *response) {
                // Received response -- get checkpoint from it:
                Checkpoint checkpoint;
                string checkpointRevID;

                if (response->isError()) {
                    if (!(response->errorDomain() == "HTTP"_sl && response->errorCode() == 404))
                        return gotError(response);
                    log("No remote checkpoint");
                } else {
                    log("Received remote checkpoint: '%.*s'", SPLAT(response->body()));
                    checkpoint = decodeCheckpoint(response->body());
                    checkpointRevID = response->property("rev"_sl).asString();
                }

                log("...got remote checkpoint: [%llu, '%s']",
                      checkpoint.localSeq, checkpoint.remoteSeq.c_str());

                // Reset mismatched checkpoints:
                if (_checkpoint.localSeq > 0 && _checkpoint.localSeq != checkpoint.localSeq) {
                    log("Local sequence mismatch: I had %llu, remote had %llu",
                          _checkpoint.localSeq, checkpoint.localSeq);
                    _checkpoint.localSeq = 0;
                }
                if (!_checkpoint.remoteSeq.empty() && _checkpoint.remoteSeq != checkpoint.remoteSeq) {
                    log("Remote sequence mismatch: I had '%s', remote had '%s'",
                          _checkpoint.remoteSeq.c_str(), checkpoint.remoteSeq.c_str());
                    _checkpoint.remoteSeq = "";
                }

                // Now we have the checkpoints! Time to start replicating:
                startReplicating();
            });
        }));
    }


    // Decodes the body of a checkpoint doc into a Checkpoint struct
    Replicator::Checkpoint Replicator::decodeCheckpoint(slice json) {
        Checkpoint c;
        if (json) {
            alloc_slice f = Encoder::convertJSON(json, nullptr);
            Dict root = Value::fromData(f).asDict();
            c.localSeq = (C4SequenceNumber) root["local"_sl].asInt();
            c.remoteSeq = asstring(root["remote"_sl].toString());
        }
        return c;
    }

    alloc_slice Replicator::encodeCheckpoint(const Checkpoint &checkpoint) {
        JSONEncoder enc;
        enc.beginDict();
        if (checkpoint.localSeq) {
            enc.writeKey("local"_sl);
            enc.writeUInt(checkpoint.localSeq);
        }
        if (!checkpoint.remoteSeq.empty()) {
            enc.writeKey("remote"_sl);
            enc.writeString(checkpoint.remoteSeq);
        }
        enc.endDict();
        return enc.finish();
    }



    void Replicator::updatePushCheckpoint(C4SequenceNumber seq) {
        _checkpoint.localSeq = seq;     //TODO: Thread-safety
        _checkpointChanged = true;
        updateCheckpoint();
    }

    void Replicator::updatePullCheckpoint(std::string seq) {
        _checkpoint.remoteSeq = seq;    // TODO: Thread-safety
        _checkpointChanged = true;
        updateCheckpoint();
    }

    void Replicator::_updateCheckpoint() {
        if (!_checkpointChanged)
            return;
        _checkpointChanged = false;
        log("Saving remote checkpoint %.*s ...", SPLAT(_checkpointID));
        auto json = encodeCheckpoint(_checkpoint);
        MessageBuilder msg("setCheckpoint"_sl);
        msg["client"_sl] = _checkpointID;
        msg << json;
        sendRequest(msg, [=](MessageIn *response) {
            if (response->isError()) {
                gotError(response);
            } else {
                // Remote checkpoint saved, so update local one:
                log("Successfully saved remote checkpoint %.*s", SPLAT(_checkpointID));
                _dbActor->setCheckpoint(json);
            }
        });
    }


    void Replicator::afterEvent() {
        // Decide whether a non-continuous active push or pull replication is done:
        if (!isBusy() && !_pushing && !_pulling && !_options.continuous
                      && connection() && !connection()->isServer()) {
            log("Replication complete! Closing connection");
            connection()->close();
        }
    }

} }
