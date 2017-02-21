//
//  Replicator.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

//  https://github.com/couchbaselabs/couchbase-lite-api/wiki/New-Replication-Protocol

#include "Replicator.hh"
#include "Pusher.hh"
#include "Puller.hh"
#include "Logging.hh"
#include "SecureDigest.hh"

using namespace std;
using namespace std::placeholders;
using namespace fleece;
using namespace fleeceapi;


namespace litecore { namespace repl {


    Replicator::Replicator(C4Database *db,
                           websocket::Provider &provider,
                           const websocket::Address &address,
                           Options options)
    :_db(db)
    ,_remoteAddress(address)
    ,_options(options)
    {
        setConnection(new Connection(_remoteAddress, provider, *this));
    }


    Replicator::Replicator(C4Database* db,
                           Connection *connection,
                           const websocket::Address &address)
    :_db(db)
    ,_remoteAddress(address)
    ,_options()
    {
        setConnection(connection);
    }


    void Replicator::setConnection(Connection *connection) {
        ReplActor::setConnection(connection);
        _pusher = new Pusher(this);
        _puller = new Puller(this);
        registerHandler("getCheckpoint",    &Replicator::handleGetCheckpoint);
        registerHandler("changes",          &Replicator::handleChanges);
    }


    void Replicator::_onConnect() {
        LogTo(SyncLog, "** BLIP Connected");
        if (_options.push || _options.pull)
            getCheckpoint();
    }

    void Replicator::_onError(int errcode, alloc_slice reason) {
        LogTo(SyncLog, "** BLIP error: %s (%d)", reason.asString().c_str(), errcode);
    }

    void Replicator::_onClose(int status, alloc_slice reason) {
        LogTo(SyncLog, "** BLIP closed: %s (status %d)", reason.asString().c_str(), status);
    }

    void Replicator::_onRequestReceived(Retained<MessageIn> msg) {
        LogToAt(SyncLog, Warning, "Received unrecognized BLIP request #%llu with Profile '%.*s', %zu bytes",
                msg->number(), SPLAT(msg->property("Profile"_sl)), msg->body().size);
    }


#pragma mark - CHECKPOINT:


    static bool isNotFoundError(C4Error err) {
        return err.domain == LiteCoreDomain && err.code == kC4ErrorNotFound;
    }


    void Replicator::getCheckpoint() {
        string checkpointID = effectiveRemoteCheckpointDocID();

        // Get the local checkpoint:
        C4Error err;
        slice s(checkpointID);
        C4RawDocument *doc = c4raw_get(_db, C4STR("checkpoints"), s, &err);
        if (!doc) {
            if (!isNotFoundError(err))
                throw "fail"; //FIX
            LogTo(SyncLog, "No local checkpoint '%s'", checkpointID.c_str());
            startReplicating();
            return;
        }

        _checkpoint = decodeCheckpoint(doc->body);
        LogTo(SyncLog, "Local checkpoint '%s' is [%llu, '%s']; getting remote ...",
              checkpointID.c_str(), _checkpoint.localSeq, _checkpoint.remoteSeq.c_str());

        MessageBuilder msg("getCheckpoint"_sl);
        msg["client"_sl] = slice(checkpointID);
        onReady(sendRequest(msg), [&](MessageIn *response) {
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
    }


    void Replicator::handleGetCheckpoint(Retained<MessageIn> request) {
        slice clientID = request->property("client"_sl);
        if (!clientID)
            return request->respondWithError("BLIP"_sl, 400);
        LogTo(SyncLog, "Request for checkpoint '%.*s'", SPLAT(clientID));
        C4Error err;
        C4RawDocument *doc = c4raw_get(_db, C4STR("peerCheckpoints"), clientID, &err);
        if (!doc) {
            if (isNotFoundError(err))
                return request->respondWithError("HTTP"_sl, 404);
            else
                return request->respondWithError("HTTP"_sl, 502);
        }

        MessageBuilder response(request);
        response["rev"_sl] = doc->meta;
        response << doc->body;
        request->respond(response);
    }


    string Replicator::effectiveRemoteCheckpointDocID() {
        if (!_remoteCheckpointDocID.empty())
            return _remoteCheckpointDocID;
        // Simplistic default value derived from db UUID and remote URL:
        C4UUID privateUUID;
        C4Error err;
        if (!c4db_getUUIDs(_db, nullptr, &privateUUID, &err))
            throw "fail";//FIX
        Encoder enc;
        enc.beginArray();
        enc.writeString({&privateUUID, sizeof(privateUUID)});
        enc.writeString(_remoteAddress);
        enc.endArray();
        alloc_slice data = enc.finish();
        SHA1 digest(data);
        return string("cp-") + slice(&digest, sizeof(digest)).base64String();
    }


    Replicator::Checkpoint Replicator::decodeCheckpoint(slice json) {
        Checkpoint c;
        if (json) {
            alloc_slice f = Encoder::convertJSON(json);
            Dict root = Value::fromData(f).asDict();
            c.localSeq = (C4SequenceNumber) root["local"_sl].asInt();
            c.remoteSeq = asstring(root["remote"_sl].toString());
        }
        return c;
    }


    // Called after the checkpoint is established.
    void Replicator::startReplicating() {
        if (_options.push)
            _pusher->start(_checkpoint.localSeq, _options.continuous);
        if (_options.pull)
            _puller->start(_checkpoint.remoteSeq, _options.continuous);
    }


#pragma mark - DATABASE:


    void Replicator::changeCallback(C4DatabaseObserver* observer, void *context) {
        ((Replicator*)context)->dbChanged();
    }


    // A request from the Pusher to send it a batch of changes. Will respond by calling gotChanges.
    void Replicator::_dbGetChanges(C4SequenceNumber since, unsigned limit, bool continuous) {
        LogTo(SyncLog, "Reading %u local changes from %llu", limit, since);
        vector<Rev> changes;
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
        options.flags |= kC4IncludeDeleted;
        auto e = c4db_enumerateChanges(_db, since, &options, &error);
        if (e) {
            changes.reserve(limit);
            while (c4enum_next(e, &error) && limit-- > 0) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                changes.emplace_back(info);
            }
            c4enum_free(e);
        }

        if (continuous && changes.empty() && !_changeObserver) {
            // Reached the end of history; now start observing for future changes
            _changeObserver = c4dbobs_create(_db, &changeCallback, this);
        }

        _pusher->gotChanges(changes, error);
        //TODO: Handle continuous mode (start observer)
    }


    void Replicator::dbChanged() {

    }


    // Sends a document revision in a "rev" request.
    void Replicator::_dbSendRevision(Rev rev, vector<string> ancestors, int maxHistory) {
        LogVerbose(SyncLog, "Sending revision '%.*s' #%.*s", SPLAT(rev.docID), SPLAT(rev.revID));
        C4Error c4err;
        auto doc = c4doc_get(_db, rev.docID, true, &c4err);
        if (!doc)
            return gotError(c4err);
        if (!c4doc_selectRevision(doc, rev.revID, true, &c4err))
            return gotError(c4err);

        alloc_slice body = doc->selectedRev.body;

        // Generate the revision history string:
        stringstream historyStream;
        for (int n = 0; n < maxHistory; ++n) {
            if (!c4doc_selectParentRevision(doc))
                break;
            string revID = slice(doc->selectedRev.revID).asString();
            if (n > 0)
                historyStream << ',';
            historyStream << revID;
            if (find(ancestors.begin(), ancestors.end(), revID) != ancestors.end())
                break;
        }
        string history = historyStream.str();

        // Now send the BLIP message:
        MessageBuilder msg("rev"_sl);
        msg.noreply = true;                 //FIX: Sometimes need a reply
        msg["id"_sl] = rev.docID;
        msg["rev"_sl] = rev.revID;
        msg["sequence"_sl] = rev.sequence;
        if (!history.empty())
            msg["history"_sl] = history;
        msg.write(body);
        sendRequest(msg);
    }


    void Replicator::handleChanges(Retained<MessageIn> req) {
        LogTo(SyncLog, "Handling 'changes' message");
        auto changes = req->JSONBody().asArray();
        if (!changes) {
            LogToAt(SyncLog, Warning, "Invalid body of 'changes' message");
            req->respondWithError("BLIP"_sl, 400);
            return;
        }

        if (req->noReply())
            return;

        LogTo(SyncLog, "Looking up %u revisions in the db ...", changes.count());
        MessageBuilder response(req);
        response["maxRevs"_sl] = c4db_getMaxRevTreeDepth(_db);
        unsigned i = 0, itemsWritten = 0, requested = 0;
        vector<alloc_slice> ancestors;
        auto &encoder = response.jsonBody();
        encoder.beginArray();
        for (auto item : changes) {
            auto change = item.asArray();
            slice docID = change[0].asString();
            slice revID = change[2].asString();
            if (!docID || !revID) {
                LogToAt(SyncLog, Warning, "Invalid entry in 'changes' message");
                return;     // ???  Should this abort the replication?
            }

            if (!_dbFindAncestors(docID, revID, ancestors)) {
                ++requested;
                while (++itemsWritten < i)
                    encoder.writeInt(0);
                encoder.beginArray();
                for (slice ancestor : ancestors)
                    encoder.writeString(ancestor);
                encoder.endArray();
            }
            ++i;
        }
        encoder.endArray();
        LogTo(SyncLog, "Responding w/request for %u revs", requested);
        req->respond(response);
    }


    // Returns true if revision exists; else returns false and sets ancestors to an array of
    // ancestor revisions I do have (empty if doc doesn't exist at all)
    bool Replicator::_dbFindAncestors(slice docID, slice revID, vector<alloc_slice> &ancestors) {
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(_db, docID, true, &err);
        bool revExists = doc && c4doc_selectRevision(doc, revID, false, &err);
        if (!revExists) {
            ancestors.resize(0);
            if (!isNotFoundError(err)) {
                gotError(err);
            } else if (doc) {
                // Revision isn't found, but look for ancestors:
                if (c4doc_selectFirstPossibleAncestorOf(doc, revID)) {
                    do {
                        ancestors.emplace_back(doc->selectedRev.revID);
                    } while (c4doc_selectNextPossibleAncestorOf(doc, revID)
                             && ancestors.size() < kMaxPossibleAncestors);
                }
            }
        }
        return revExists;
    }


} }
