//
//  DBActor.cc
//  LiteCore
//
//  Created by Jens Alfke on 2/21/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "DBActor.hh"
#include "Pusher.hh"
#include "FleeceCpp.hh"
#include "BLIPConnection.hh"
#include "Message.hh"
#include "StringUtil.hh"
#include "SecureDigest.hh"
#include "c4.hh"
#include "c4Document+Fleece.h"

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    static constexpr slice kLocalCheckpointStore = "checkpoints"_sl;
    static constexpr slice kPeerCheckpointStore  = "peerCheckpoints"_sl;

    static bool isNotFoundError(C4Error err) {
        return err.domain == LiteCoreDomain && err.code == kC4ErrorNotFound;
    }


    DBActor::DBActor(Connection *connection,
                     C4Database *db,
                     const websocket::Address &remoteAddress,
                     Options options)
    :ReplActor(connection, options, string("DB:") + connection->name())
    ,_db(db)
    ,_remoteAddress(remoteAddress)
    {
        registerHandler("getCheckpoint",    &DBActor::handleGetCheckpoint);
        registerHandler("setCheckpoint",    &DBActor::handleSetCheckpoint);
    }


#pragma mark - CHECKPOINTS:


    // Implementation of public getCheckpoint(): Reads the local checkpoint & calls the callback
    void DBActor::_getCheckpoint(CheckpointCallback callback) {
        alloc_slice checkpointID(effectiveRemoteCheckpointDocID());
        C4Error err;
        c4::ref<C4RawDocument> doc( c4raw_get(_db,
                                              kLocalCheckpointStore,
                                              checkpointID,
                                              &err) );
        alloc_slice body;
        if (doc)
            body = alloc_slice(doc->body);
        else if (isNotFoundError(err))
            err = {};
        callback(checkpointID, body, err);
    }


    void DBActor::_setCheckpoint(alloc_slice data) {
        alloc_slice checkpointID(effectiveRemoteCheckpointDocID());
        C4Error err;
        log("Saving local checkpoint %.*s to db", SPLAT(checkpointID));
        if (!c4raw_put(_db, kLocalCheckpointStore, checkpointID, nullslice, data, &err))
            gotError(err);
    }


    // Computes the ID of the checkpoint document.
    slice DBActor::effectiveRemoteCheckpointDocID() {
        if (_remoteCheckpointDocID.empty()) {
            // Simplistic default value derived from db UUID and remote URL:
            C4UUID privateUUID;
            C4Error err;
            if (!c4db_getUUIDs(_db, nullptr, &privateUUID, &err))
                throw "fail";//FIX
            fleeceapi::Encoder enc;
            enc.beginArray();
            enc.writeString({&privateUUID, sizeof(privateUUID)});
            enc.writeString(_remoteAddress);
            enc.endArray();
            alloc_slice data = enc.finish();
            SHA1 digest(data);
            _remoteCheckpointDocID = string("cp-") + slice(&digest, sizeof(digest)).base64String();
        }
        return slice(_remoteCheckpointDocID);
    }


    bool DBActor::getPeerCheckpointDoc(MessageIn* request, bool getting,
                                       slice &checkpointID, c4::ref<C4RawDocument> &doc) {
        checkpointID = request->property("client"_sl);
        if (!checkpointID) {
            request->respondWithError("BLIP"_sl, 400);
            return false;
        }
        log("Request to %s checkpoint '%.*s'",
            (getting ? "get" : "set"), SPLAT(checkpointID));

        C4Error err;
        doc = c4raw_get(_db, kPeerCheckpointStore, checkpointID, &err);
        if (!doc) {
            int status = isNotFoundError(err) ? 404 : 502;
            if (getting || (status != 404)) {
                request->respondWithError("HTTP"_sl, status);
                return false;
            }
        }
        return true;
    }


    // Handles a "getCheckpoint" request by looking up a peer checkpoint.
    void DBActor::handleGetCheckpoint(Retained<MessageIn> request) {
        c4::ref<C4RawDocument> doc;
        slice checkpointID;
        if (!getPeerCheckpointDoc(request, true, checkpointID, doc))
            return;
        MessageBuilder response(request);
        response["rev"_sl] = doc->meta;
        response << doc->body;
        request->respond(response);
    }


    // Handles a "setCheckpoint" request by storing a peer checkpoint.
    void DBActor::handleSetCheckpoint(Retained<MessageIn> request) {
        C4Error err;
        c4::Transaction t(_db);
        if (!t.begin(&err))
            request->respondWithError("HTTP"_sl, 502);

        // Get the existing raw doc so we can check its revID:
        slice checkpointID;
        c4::ref<C4RawDocument> doc;
        if (!getPeerCheckpointDoc(request, false, checkpointID, doc))
            return;

        slice actualRev;
        unsigned long generation = 0;
        if (doc) {
            actualRev = (slice)doc->meta;
            char *end;
            generation = strtol((const char*)actualRev.buf, &end, 10);  //FIX: can fall off end
        }

        // Check for conflict:
        if (request->property("rev"_sl) != actualRev)
            return request->respondWithError("HTTP"_sl, 409);

        // Generate new revID:
        char newRevBuf[30];
        slice rev = slice(newRevBuf, sprintf(newRevBuf, "%lu-", ++generation));

        // Save:
        if (!c4raw_put(_db, kPeerCheckpointStore, checkpointID, rev, request->body(), &err)
                || !t.commit(&err)) {
            return request->respondWithError("HTTP"_sl, 502);
        }

        // Success!
        MessageBuilder response(request);
        response["rev"_sl] = rev;
        request->respond(response);
    }


#pragma mark - CHANGES:


    void DBActor::getChanges(C4SequenceNumber since, unsigned limit, bool cont, Pusher *pusher) {
        enqueue(&DBActor::_getChanges, since, limit, cont, Retained<Pusher>(pusher));
    }

    
    // A request from the Pusher to send it a batch of changes. Will respond by calling gotChanges.
    void DBActor::_getChanges(C4SequenceNumber since, unsigned limit, bool continuous,
                              Retained<Pusher> pusher) {
        log("Reading %u local changes from %llu", limit, since);
        vector<Rev> changes;
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        options.flags &= ~kC4IncludeBodies;
        options.flags |= kC4IncludeDeleted;
        c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(_db, since, &options, &error);
        if (e) {
            changes.reserve(limit);
            while (c4enum_next(e, &error) && limit-- > 0) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                changes.emplace_back(info);
            }
        }

        if (continuous && changes.empty() && !_changeObserver) {
            // Reached the end of history; now start observing for future changes
            _changeObserver = c4dbobs_create(_db,
                                             [](C4DatabaseObserver* observer, void *context) {
                                                 ((DBActor*)context)->dbChanged();
                                             },
                                             this);
        }

        pusher->gotChanges(changes, error);
        //TODO: Handle continuous mode (start observer)
    }


    // Callback from the C4DatabaseObserver when the database has changed
    void DBActor::dbChanged() {
        // TODO: Send new changes
    }


    // Called by the Pusher; it passes on the "changes" message
    void DBActor::_findOrRequestRevs(Retained<MessageIn> req,
                                     function<void(vector<alloc_slice>)> callback) {
        // Iterate over the array in the message, seeing whether I have each revision:
        auto changes = req->JSONBody().asArray();
        log("Looking up %u revisions in the db ...", changes.count());
        MessageBuilder response(req);
        response["maxRevs"_sl] = c4db_getMaxRevTreeDepth(_db);
        vector<alloc_slice> requestedSequences;
        unsigned i = 0, itemsWritten = 0, requested = 0;
        vector<alloc_slice> ancestors;
        auto &encoder = response.jsonBody();
        encoder.beginArray();
        for (auto item : changes) {
            auto change = item.asArray();
            slice docID = change[1].asString();
            slice revID = change[2].asString();
            if (!docID || !revID) {
                warn("Invalid entry in 'changes' message");
                return;     // ???  Should this abort the replication?
            }

            if (!findAncestors(docID, revID, ancestors)) {
                // I don't have this revision, so request it:
                ++requested;
                while (++itemsWritten < i)
                    encoder.writeInt(0);
                encoder.beginArray();
                for (slice ancestor : ancestors)
                    encoder.writeString(ancestor);
                encoder.endArray();

                if (callback) {
                    alloc_slice sequence(change[0].toString()); //FIX: Should quote strings
                    if (sequence)
                        requestedSequences.push_back(sequence);
                    else
                        warn("Empty/invalid sequence in 'changes' message");
                }
            }
            ++i;
        }
        encoder.endArray();

        if (callback)
            callback(requestedSequences);

        log("Responding w/request for %u revs", requested);
        req->respond(response);
    }


#pragma mark - REVISIONS:


    // Sends a document revision in a "rev" request.
    void DBActor::_sendRevision(Rev rev,
                                vector<string> ancestors,
                                int maxHistory,
                                function<void(Retained<blip::MessageIn>)> onReply)
    {
        if (!connection())
            return;
        LogVerbose(SyncLog, "Sending revision '%.*s' #%.*s", SPLAT(rev.docID), SPLAT(rev.revID));
        C4Error c4err;
        c4::ref<C4Document> doc = c4doc_get(_db, rev.docID, true, &c4err);
        if (!doc)
            return gotError(c4err);
        if (!c4doc_selectRevision(doc, rev.revID, true, &c4err))
            return gotError(c4err);

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
        msg.noreply = !onReply;
        msg["id"_sl] = rev.docID;
        msg["rev"_sl] = rev.revID;
        msg["sequence"_sl] = rev.sequence;
        if (doc->selectedRev.flags & kRevDeleted)
            msg["del"_sl] = "1"_sl;
        if (!history.empty())
            msg["history"_sl] = history;

        auto root = fleeceapi::Value::fromTrustedData(doc->selectedRev.body);
        assert(root);
        msg.jsonBody().setSharedKeys(c4db_getFLSharedKeys(_db));
        msg.jsonBody().writeValue(root);

        sendRequest(msg, onReply);
    }


    // Implementation of insertRevision(). Adds a rev to the database and calls the callback.
    void DBActor::_insertRevision(Rev rev, alloc_slice historyBuf, alloc_slice body,
                                  std::function<void(C4Error)> callback)
    {
        log("Inserting rev {'%.*s' #%.*s}", SPLAT(rev.docID), SPLAT(rev.revID));
        vector<C4String> history;
        history.reserve(10);
        history.push_back(rev.revID);
        for (const void *pos=historyBuf.buf, *end = historyBuf.end(); pos < end;) {
            auto comma = slice(pos, end).findByteOrEnd(',');
            history.push_back(slice(pos, comma));
            pos = comma + 1;
        }
        C4DocPutRequest put = {};
        put.body = body;
        put.docID = rev.docID;
        put.revFlags = rev.deleted ? kRevDeleted : 0;
        put.existingRevision = true;
        put.allowConflict = true;
        put.history = history.data();
        put.historyCount = history.size();
        put.save = true;

        C4Error err;
        c4::Transaction transaction(_db);
        if (transaction.begin(&err)) {
            c4::ref<C4Document> doc = c4doc_put(_db, &put, nullptr, &err);
            if (doc && transaction.commit(&err)) {
                err = {}; // success
            }
        }
        if (callback)
            callback(err);
    }


    // Returns true if revision exists; else returns false and sets ancestors to an array of
    // ancestor revisions I do have (empty if doc doesn't exist at all)
    bool DBActor::findAncestors(slice docID, slice revID, vector<alloc_slice> &ancestors) {
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
