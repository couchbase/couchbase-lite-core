//
//  Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Replicator.hh"
#include "Pusher.hh"
#include "Puller.hh"
#include "Logging.hh"
#include "Fleece.hh"
#include "SecureDigest.hh"
#include "c4.h"

using namespace std;
using namespace fleece;


namespace litecore { namespace repl {


    LogDomain SyncLog("Sync");


    Replicator::Replicator(C4Database *db,
                           WebSocketProvider &provider,
                           const WebSocketAddress &&address,
                           Options options)
    :_db(db)
    ,_remoteAddress(address)
    ,_options(options)
    {
        _connection = new Connection(WebSocketAddress("localhost", 1234), provider, *this);
    }


    void Replicator::_onConnect() {
        LogTo(SyncLog, "** BLIP Connected");
        getCheckpoint();
    }

    void Replicator::_onError(int errcode, slice reason) {
        LogTo(SyncLog, "** BLIP error: %s (%d)", reason.asString().c_str(), errcode);
    }

    void Replicator::_onClose(int status, slice reason) {
        LogTo(SyncLog, "** BLIP closed: %s (status %d)", reason.asString().c_str(), status);
    }

    void Replicator::_onRequestReceived(MessageIn *msg) {
        LogTo(SyncLog, "** BLIP request #%llu received: %zu bytes", msg->number(), msg->body().size);
    }


    void Replicator::gotError(const MessageIn* msg) {
        // TODO
        LogToAt(SyncLog, Error, "Got error response: %.*s %d",
                (int)msg->errorDomain().size, msg->errorDomain().buf, msg->errorCode());
    }

#pragma mark - CHECKPOINT:


    string Replicator::effectiveRemoteCheckpointDocID() {
        if (!_remoteCheckpointDocID.empty())
            return _remoteCheckpointDocID;
        // Simplistic default value derived from db UUID and remote URL:
        C4UUID privateUUID;
        C4Error err;
        if (!c4db_getUUIDs(_db, nullptr, &privateUUID, &err))
            throw "fail";//FIX
        Encoder enc;
        enc << slice{&privateUUID, sizeof(privateUUID)} << (string)_remoteAddress;
        SHA1 digest(enc.extractOutput());
        return string("cp-") + slice(&digest, sizeof(digest)).base64String();
    }


    Replicator::Checkpoint Replicator::decodeCheckpoint(slice json) {
        Checkpoint c;
        if (json) {
            alloc_slice f = JSONConverter::convertJSON(json);
            const Dict *root = Value::fromData(f)->asDict();
            if (root) {
                auto item = root->get("local"_sl);
                if (item)
                    c.localSeq = (sequence_t) item->asInt();
                item = root->get("remote"_sl);
                if (item)
                    c.remoteSeq = item->toString().asString();
            }
        }
        return c;
    }


    void Replicator::getCheckpoint() {
        string checkpointID = effectiveRemoteCheckpointDocID();
        MessageBuilder msg({ {"Profile"_sl, "getCheckpoint"_sl},
                             {"client"_sl, slice(checkpointID)} });
        onReady(sendRequest(msg), [&](MessageIn *response) {
            // Received response -- get checkpoint from it:
            Checkpoint checkpoint;
            string checkpointRevID;

            if (response->isError()) {
                if (!(response->errorDomain() == "HTTP"_sl && response->errorCode() == 404))
                    return gotError(response);
                LogTo(SyncLog, "No remote checkpoint");
            } else {
                LogTo(SyncLog, "Received remote checkpoint: %.*s",
                      (int)response->body().size, response->body().buf);
                checkpoint = decodeCheckpoint(response->body());
                checkpointRevID = response->property("rev"_sl).asString();
            }

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

        // While waiting for the response, get the local checkpoint:
        C4Error err;
        slice s(checkpointID);
        C4RawDocument *doc = c4raw_get(_db, C4STR("checkpoints"), {s.buf, s.size}, &err);
        if (doc)
            _checkpoint = decodeCheckpoint(slice{doc->body.buf, doc->body.size});
        else if (err.code != 0)
            throw "fail"; //FIX
    }


    void Replicator::startReplicating() {
        if (_options.push) {
            _pusher = new Pusher(this, _options.continuous, _checkpoint.localSeq);
            _pusher->start();
        }
        if (_options.pull) {
            _puller = new Puller(this, _options.continuous, _checkpoint.remoteSeq);
            _puller->start();
        }
    }


} }
