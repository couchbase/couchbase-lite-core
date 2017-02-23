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
        // Next is _onConnect...
    }

    Replicator::Replicator(C4Database *db,
                           websocket::Provider &provider,
                           const websocket::Address &address,
                           Options options)
    :Replicator(db, address, options, new Connection(address, provider, *this))
    { }

    Replicator::Replicator(C4Database *db,
                           blip::Connection *connection,
                           const websocket::Address& address)
    :Replicator(db, address, Options{}, connection)
    { }


    void Replicator::_taskComplete(bool isPush) {
        if (isPush)
            _pushing = false;
        else
            _pulling = false;
        if (!_pushing && !_pulling && !_options.continuous && connection()) {
            LogTo(SyncLog, "Replication complete!");
            connection()->close();
        }
    }


    void Replicator::_connectionClosed() {
        ReplActor::_connectionClosed();
        _dbActor->connectionClosed();
        _pusher->connectionClosed();
        _puller->connectionClosed();
    }


#pragma mark - BLIP DELEGATE:


    void Replicator::_onConnect() {
        LogTo(SyncLog, "** BLIP Connected");
        if (_options.push || _options.pull)
            getCheckpoints();
    }

    void Replicator::_onError(int errcode, alloc_slice reason) {
        LogTo(SyncLog, "** BLIP error: %s (%d)", reason.asString().c_str(), errcode);
        connectionClosed();
    }

    void Replicator::_onClose(bool normalClose, int status, alloc_slice reason) {
        LogTo(SyncLog, "** BLIP %s: %s (status %d)",
              (normalClose ? "closed" : "disconnected"),
              reason.asString().c_str(), status);
        connectionClosed();
    }

    void Replicator::_onRequestReceived(Retained<MessageIn> msg) {
        LogToAt(SyncLog, Warning, "Received unrecognized BLIP request #%llu with Profile '%.*s', %zu bytes",
                msg->number(), SPLAT(msg->property("Profile"_sl)), msg->body().size);
    }


#pragma mark - CHECKPOINT:


    void Replicator::getCheckpoints() {
        // Get the local checkpoint:
        _dbActor->getCheckpoint(asynchronize([this](alloc_slice checkpointID,
                                                    alloc_slice data,
                                                    alloc_slice rev,
                                                    C4Error err) {
            if (!data) {
                if (err.code) {
                    gotError(err);
                } else {
                    // Skip getting remote checkpoint since there's no local one.
                    LogTo(SyncLog, "No local checkpoint '%.*s'", SPLAT(checkpointID));
                    startReplicating();
                }
                return;
            }

            _checkpoint = decodeCheckpoint(data);
            LogTo(SyncLog, "Local checkpoint '%.*s' is [%llu, '%s']; getting remote ...",
                  SPLAT(checkpointID), _checkpoint.localSeq, _checkpoint.remoteSeq.c_str());

            MessageBuilder msg("getCheckpoint"_sl);
            msg["client"_sl] = slice(checkpointID);
            onReady(sendRequest(msg), [this](MessageIn *response) {
                // Received response -- get checkpoint from it:
                Checkpoint checkpoint;
                string checkpointRevID;

                if (response->isError()) {
                    if (!(response->errorDomain() == "HTTP"_sl && response->errorCode() == 404))
                        return gotError(response);
                    LogTo(SyncLog, "No remote checkpoint");
                } else {
                    LogTo(SyncLog, "Received remote checkpoint: '%.*s'", SPLAT(response->body()));
                    checkpoint = decodeCheckpoint(response->body());
                    checkpointRevID = response->property("rev"_sl).asString();
                }

                LogTo(SyncLog, "...got remote checkpoint: [%llu, '%s']",
                      checkpoint.localSeq, checkpoint.remoteSeq.c_str());

                // Reset mismatched checkpoints:
                if (_checkpoint.localSeq > 0 && _checkpoint.localSeq != checkpoint.localSeq) {
                    LogTo(SyncLog, "Local sequence mismatch: I had %llu, remote had %llu",
                          _checkpoint.localSeq, checkpoint.localSeq);
                    _checkpoint.localSeq = 0;
                }
                if (!_checkpoint.remoteSeq.empty() && _checkpoint.remoteSeq != checkpoint.remoteSeq) {
                    LogTo(SyncLog, "Remote sequence mismatch: I had '%s', remote had '%s'",
                          _checkpoint.remoteSeq.c_str(), checkpoint.remoteSeq.c_str());
                    _checkpoint.remoteSeq = "";
                }

                // Now we have the checkpoints! Time to start replicating:
                startReplicating();
            });
        }));
    }


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


    // Called after the checkpoint is established.
    void Replicator::startReplicating() {
        if (_options.push)
            _pusher->start(_checkpoint.localSeq, _options);
        if (_options.pull)
            _puller->start(_checkpoint.remoteSeq, _options);
    }

} }
