//
// DBWorker.cc
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

#include "DBWorker.hh"
#include "Pusher.hh"
#include "IncomingRev.hh"
#include "FleeceCpp.hh"
#include "StringUtil.hh"
#include "SecureDigest.hh"
#include "Stopwatch.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "c4Private.h"
#include "BLIP.hh"
#include <chrono>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore::blip;

namespace litecore { namespace repl {

    static constexpr slice kLocalCheckpointStore = "checkpoints"_sl;
    static constexpr slice kPeerCheckpointStore  = "peerCheckpoints"_sl;

    static constexpr auto kInsertionDelay = chrono::milliseconds(50);


    static bool isNotFoundError(C4Error err) {
        return err.domain == LiteCoreDomain && err.code == kC4ErrorNotFound;
    }

    static bool hasConflict(C4Document *doc) {
        return c4doc_selectCurrentRevision(doc)
            && c4doc_selectNextLeafRevision(doc, false, false, nullptr);
    }


    DBWorker::DBWorker(Connection *connection,
                     Replicator *replicator,
                     C4Database *db,
                     const websocket::Address &remoteAddress,
                     Options options)
    :Worker(connection, replicator, options, "DB")
    ,_db(c4db_retain(db))
    ,_blobStore(c4db_getBlobStore(db, nullptr))
    ,_remoteAddress(remoteAddress)
    ,_insertTimer(bind(&DBWorker::insertRevisionsNow, this))
    {
        registerHandler("getCheckpoint",    &DBWorker::handleGetCheckpoint);
        registerHandler("setCheckpoint",    &DBWorker::handleSetCheckpoint);
    }


    void DBWorker::_connectionClosed() {
        Worker::_connectionClosed();
        _pusher = nullptr;                      // breaks ref-cycle
        _changeObserver = nullptr;
    }


    // Returns a string that uniquely identifies the remote database; by default its URL,
    // or the 'remoteUniqueID' option if that's present (for P2P dbs without stable URLs.)
    string DBWorker::remoteDBIDString() const {
        slice uniqueID = _options.properties[kC4ReplicatorOptionRemoteDBUniqueID].asString();
        if (uniqueID)
            return string(uniqueID);
        return string(_remoteAddress);
    }


    void DBWorker::_setCookie(alloc_slice setCookieHeader) {
        C4Error err;
        if (c4db_setCookie(_db, setCookieHeader, slice(_remoteAddress.hostname), &err)) {
            logVerbose("Set cookie: `%.*s`", SPLAT(setCookieHeader));
        } else {
            alloc_slice message = c4error_getMessage(err);
            warn("Unable to set cookie `%.*s`: %.*s (%d/%d)",
                 SPLAT(setCookieHeader), SPLAT(message), err.domain, err.code);
        }
    }


#pragma mark - CHECKPOINTS:


    // Reads the local checkpoint & calls the callback; called by Replicator::getCheckpoints()
    void DBWorker::_getCheckpoint(CheckpointCallback callback) {
        alloc_slice body;
        C4Error err;
        alloc_slice checkpointID(effectiveRemoteCheckpointDocID(&err));
        if (checkpointID) {
            c4::ref<C4RawDocument> doc( c4raw_get(_db,
                                                  kLocalCheckpointStore,
                                                  checkpointID,
                                                  &err) );
            if (doc)
                body = alloc_slice(doc->body);
            else if (isNotFoundError(err))
                err = {};
        }

        if (_options.pull > kC4Passive || _options.push > kC4Passive) {
            string key = remoteDBIDString();
            _remoteDBID = c4db_getRemoteDBID(_db, slice(key), true, &err);
            if (_remoteDBID) {
                logVerbose("Remote-DB ID %u found for target <%s>", _remoteDBID, key.c_str());
            } else {
                warn("Couldn't get remote-DB ID for target <%s>: error %d/%d",
                     key.c_str(), err.domain, err.code);
                body = nullslice;     // Let caller know there's a fatal error
            }
        }

        bool dbIsEmpty = c4db_getLastSequence(_db) == 0;
        callback(checkpointID, body, dbIsEmpty, err);
    }


    void DBWorker::_setCheckpoint(alloc_slice data, std::function<void()> onComplete) {
        C4Error err;
        auto checkpointID = effectiveRemoteCheckpointDocID(&err);
        if (checkpointID && c4raw_put(_db, kLocalCheckpointStore, checkpointID, nullslice, data, &err))
            log("Saved local checkpoint %.*s to db", SPLAT(checkpointID));
        else
            gotError(err);
        onComplete();
    }


    // Writes a Value to an Encoder, substituting null if the value is an empty array.
    static void writeValueOrNull(Encoder &enc, Value val) {
        auto a = val.asArray();
        if (!val || (a && a.empty()))
            enc.writeNull();
        else
            enc.writeValue(val);
    }


    // Computes the ID of the checkpoint document.
    slice DBWorker::effectiveRemoteCheckpointDocID(C4Error *err) {
        if (_remoteCheckpointDocID.empty()) {
            // Derive docID from from db UUID, remote URL, channels, filter, and docIDs.
            C4UUID privateUUID;
            if (!c4db_getUUIDs(_db, nullptr, &privateUUID, err))
                return nullslice;
            Array channels = _options.channels();
            Value filter = _options.properties[kC4ReplicatorOptionFilter];
            Value filterParams = _options.properties[kC4ReplicatorOptionFilterParams];
            Array docIDs = _options.docIDs();

            // Compute the ID by writing the values to a Fleece array, then taking a SHA1 digest:
            fleeceapi::Encoder enc;
            enc.beginArray();
            enc.writeString({&privateUUID, sizeof(privateUUID)});
            enc.writeString(remoteDBIDString());
            if (!channels.empty() || !docIDs.empty() || filter) {
                // Optional stuff:
                writeValueOrNull(enc, channels);
                writeValueOrNull(enc, filter);
                writeValueOrNull(enc, filterParams);
                writeValueOrNull(enc, docIDs);
            }
            enc.endArray();
            alloc_slice data = enc.finish();
            SHA1 digest(data);
            _remoteCheckpointDocID = string("cp-") + slice(&digest, sizeof(digest)).base64String();
            logVerbose("Checkpoint doc ID = %s", _remoteCheckpointDocID.c_str());
        }
        return slice(_remoteCheckpointDocID);
    }


    bool DBWorker::getPeerCheckpointDoc(MessageIn* request, bool getting,
                                       slice &checkpointID, c4::ref<C4RawDocument> &doc) {
        checkpointID = request->property("client"_sl);
        if (!checkpointID) {
            request->respondWithError({"BLIP"_sl, 400, "missing checkpoint ID"_sl});
            return false;
        }
        log("Request to %s checkpoint '%.*s'",
            (getting ? "get" : "set"), SPLAT(checkpointID));

        C4Error err;
        doc = c4raw_get(_db, kPeerCheckpointStore, checkpointID, &err);
        if (!doc) {
            int status = isNotFoundError(err) ? 404 : 502;
            if (getting || (status != 404)) {
                request->respondWithError({"HTTP"_sl, status});
                return false;
            }
        }
        return true;
    }


    // Handles a "getCheckpoint" request by looking up a peer checkpoint.
    void DBWorker::handleGetCheckpoint(Retained<MessageIn> request) {
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
    void DBWorker::handleSetCheckpoint(Retained<MessageIn> request) {
        C4Error err;
        c4::Transaction t(_db);
        if (!t.begin(&err))
            request->respondWithError(c4ToBLIPError(err));

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
            return request->respondWithError({"HTTP"_sl, 409, "revision ID mismatch"_sl});

        // Generate new revID:
        char newRevBuf[30];
        slice rev = slice(newRevBuf, sprintf(newRevBuf, "%lu-cc", ++generation));

        // Save:
        if (!c4raw_put(_db, kPeerCheckpointStore, checkpointID, rev, request->body(), &err)
                || !t.commit(&err)) {
            return request->respondWithError(c4ToBLIPError(err));
        }

        // Success!
        MessageBuilder response(request);
        response["rev"_sl] = rev;
        request->respond(response);
    }


#pragma mark - CHANGES:


    static bool passesDocIDFilter(const DocIDSet &docIDs, slice docID) {
        return !docIDs || (docIDs->find(docID.asString()) != docIDs->end());
    }


    void DBWorker::getChanges(const GetChangesParams &params, Pusher *pusher)
    {
        enqueue(&DBWorker::_getChanges, params, Retained<Pusher>(pusher));
    }

    
    // A request from the Pusher to send it a batch of changes. Will respond by calling gotChanges.
    void DBWorker::_getChanges(GetChangesParams p, Retained<Pusher> pusher)
    {
        if (!connection())
            return;
        log("Reading up to %u local changes since #%llu", p.limit, p.since);
        _getForeignAncestors = p.getForeignAncestors;
        _skipForeignChanges = p.skipForeign;
        if (_maxPushedSequence == 0)
            _maxPushedSequence = p.since;
        C4SequenceNumber latestChangeSequence = _maxPushedSequence;

        // Run a by-sequence enumerator to find the changed docs:
        vector<Rev> changes;
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (!p.getForeignAncestors)
            options.flags &= ~kC4IncludeBodies;
        if (!p.skipDeleted)
            options.flags |= kC4IncludeDeleted;
        c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(_db, p.since, &options, &error);
        if (e) {
            changes.reserve(p.limit);
            while (c4enum_next(e, &error) && p.limit > 0) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                // (There's very similar code below in dbChanged; keep them in sync)
                latestChangeSequence = info.sequence;
                if (!passesDocIDFilter(p.docIDs, info.docID))
                    continue;       // reject rev: not in filter

                c4::ref<C4Document> doc;
                if (_getForeignAncestors) {
                    doc = c4enum_getDocument(e, &error);
                    if (!doc) {
                        gotDocumentError(info.docID, error, true, false);
                        continue;   // reject rev: error getting doc
                    }
                }
                if (addChangeToList(info, doc, changes))
                    --p.limit;
            }
        }
        _maxPushedSequence = latestChangeSequence;

        markRevsSynced(changes, nullptr);
        pusher->gotChanges(changes, error);

        if (p.continuous && p.limit > 0 && !_changeObserver) {
            // Reached the end of history; now start observing for future changes
            _pusher = pusher;
            _pushDocIDs = p.docIDs;
            _changeObserver = c4dbobs_create(_db,
                                             [](C4DatabaseObserver* observer, void *context) {
                                                 auto self = (DBWorker*)context;
                                                 self->enqueue(&DBWorker::dbChanged);
                                             },
                                             this);
            logDebug("Started DB observer");
        }
    }


    // Callback from the C4DatabaseObserver when the database has changed
    void DBWorker::dbChanged() {
        static const uint32_t kMaxChanges = 100;
        C4DatabaseChange c4changes[kMaxChanges];
        bool external;
        uint32_t nChanges;
        vector<Rev> changes;
        while (true) {
            nChanges = c4dbobs_getChanges(_changeObserver, c4changes, kMaxChanges, &external);
            if (nChanges == 0)
                break;
            log("Notified of %u db changes #%llu ... #%llu",
                nChanges, c4changes[0].sequence, c4changes[nChanges-1].sequence);
            C4SequenceNumber latestChangeSequence = _maxPushedSequence;
            changes.clear();
            C4DatabaseChange *c4change = c4changes;
            for (uint32_t i = 0; i < nChanges; ++i, ++c4change) {
                C4DocumentInfo info {0, c4change->docID, c4change->revID,
                                     c4change->sequence, c4change->bodySize};
                // (There's very similar code above in _getChanges; keep them in sync)
                latestChangeSequence = info.sequence;
                if (!passesDocIDFilter(_pushDocIDs, info.docID))
                    continue;

                c4::ref<C4Document> doc;
                if (_getForeignAncestors) {
                    C4Error error;
                    doc = c4doc_get(_db, info.docID, true, &error);
                    if (!doc) {
                        gotDocumentError(info.docID, error, true, false);
                        continue;   // reject rev: error getting doc
                    }
                    if (slice(doc->revID) != slice(info.revID))
                        continue;   // ignore rev: there's a newer one already
                }
                addChangeToList(info, doc, changes);
                // Note: we send tombstones even if the original getChanges() call specified
                // skipDeletions. This is intentional; skipDeletions applies only to the initial
                // dump of existing docs, not to 'live' changes.
            }
            _maxPushedSequence = latestChangeSequence;

            if (!changes.empty()) {
                markRevsSynced(changes, nullptr);
                _pusher->gotChanges(changes, {});
            }
        }
    }


    // Common subroutine of _getChanges and dbChanged that adds a document to a list of Revs.
    bool DBWorker::addChangeToList(const C4DocumentInfo &info, C4Document *doc, vector<Rev> &changes) {
        alloc_slice remoteRevID;
        if (_getForeignAncestors) {
            // For proposeChanges, find the nearest foreign ancestor of the current rev:
            remoteRevID = getRemoteRevID(doc);
            if (_skipForeignChanges && remoteRevID == slice(info.revID))
                return false;   // reject rev: it's foreign
        }
        changes.emplace_back(info, remoteRevID);
        return true;
    }


    // For proposeChanges, find the latest ancestor of the current rev that is known to the server.
    // This is a rev that's either marked as foreign (came from the server), or whose sequence is
    // in the range that's already been pushed to the server.
    alloc_slice DBWorker::getRemoteRevID(C4Document* doc) {
        Assert(_remoteDBID);
        c4::sliceResult foreignAncestor( c4doc_getRemoteAncestor(doc, _remoteDBID) );
        do {
            slice rev(doc->selectedRev.revID);
            if (rev == foreignAncestor) {
                return alloc_slice(foreignAncestor);
            } else if (doc->selectedRev.sequence <= _maxPushedSequence) {
                return alloc_slice(rev);
            }
        } while (c4doc_selectParentRevision(doc));
        return alloc_slice();
    }


    // Mark all of these revs as synced, which flags them as kRevKeepBody if they're still current.
    // This is actually premature because those revs haven't been pushed yet; but if we
    // wait until after the push, the doc may have been updated again and the body of the
    // pushed revision lost. By doing it early we err on the side of correctness and may
    // save some revision bodies unnecessarily, which isn't that bad.
    bool DBWorker::markRevsSynced(const vector<Rev> changes, C4Error *outError) {
        if (changes.empty())
            return true;
        c4::Transaction t(_db);
        if (!t.begin(outError))
            return false;
        for (auto i = changes.begin(); i != changes.end(); ++i)
            c4db_markSynced(_db, i->docID, i->sequence);
        return t.commit(outError);
    }


    // Called by the Puller; handles a "changes" or "proposeChanges" message by checking which of
    // the changes don't exist locally, and returning a bit-vector indicating them.
    void DBWorker::_findOrRequestRevs(Retained<MessageIn> req,
                                     function<void(vector<bool>)> callback) {
        // Iterate over the array in the message, seeing whether I have each revision:
        bool proposed = (req->property("Profile"_sl) == "proposeChanges"_sl);
        auto changes = req->JSONBody().asArray();
        if (willLog() && !changes.empty()) {
            if (proposed) {
                log("Looking up %u proposed revisions in the db", changes.count());
            } else {
                alloc_slice firstSeq(changes[0].asArray()[0].toString());
                alloc_slice lastSeq (changes[changes.count()-1].asArray()[0].toString());
                log("Looking up %u revisions in the db (seq '%.*s'..'%.*s')",
                    changes.count(), SPLAT(firstSeq), SPLAT(lastSeq));
            }
        }

        MessageBuilder response(req);
        response.compressed = true;
        response["maxHistory"_sl] = c4db_getMaxRevTreeDepth(_db);
        response["blobs"_sl] = "true"_sl;
        vector<bool> whichRequested(changes.count());
        unsigned itemsWritten = 0, requested = 0;
        vector<alloc_slice> ancestors;
        auto &encoder = response.jsonBody();
        encoder.beginArray();
        int i = -1;
        for (auto item : changes) {
            ++i;
            // Look up each revision in the `req` list:
            auto change = item.asArray();
            slice docID = change[proposed ? 0 : 1].asString();
            slice revID = change[proposed ? 1 : 2].asString();
            if (docID.size == 0 || revID.size == 0) {
                warn("Invalid entry in 'changes' message");
                continue;     // ???  Should this abort the replication?
            }

            if (proposed) {
                // "proposeChanges" entry: [docID, revID, parentRevID?, bodySize?]
                slice parentRevID = change[2].asString();
                if (parentRevID.size == 0)
                    parentRevID = nullslice;
                int status = findProposedChange(docID, revID, parentRevID);
                if (status == 0) {
                    ++requested;
                    whichRequested[i] = true;
                } else {
                    log("Rejecting proposed change '%.*s' #%.*s, parent #%.*s (status %d)",
                        SPLAT(docID), SPLAT(revID), SPLAT(parentRevID), status);
                    while (itemsWritten++ < i)
                        encoder.writeInt(0);
                    encoder.writeInt(status);
                }

            } else {
                // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
                if (!findAncestors(docID, revID, ancestors)) {
                    // I don't have this revision, so request it:
                    ++requested;
                    whichRequested[i] = true;

                    while (itemsWritten++ < i)
                        encoder.writeInt(0);
                    encoder.beginArray();
                    for (slice ancestor : ancestors)
                        encoder.writeString(ancestor);
                    encoder.endArray();
                }
            }
        }
        encoder.endArray();

        if (callback)
            callback(whichRequested);

        log("Responding w/request for %u revs", requested);
        req->respond(response);
    }


    // Returns true if revision exists; else returns false and sets ancestors to an array of
    // ancestor revisions I do have (empty if doc doesn't exist at all)
    bool DBWorker::findAncestors(slice docID, slice revID, vector<alloc_slice> &ancestors) {
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(_db, docID, true, &err);
        if (doc && c4doc_selectRevision(doc, revID, false, &err)) {
            // I already have this revision. Make sure it's marked as current for this remote:
            if (_remoteDBID) {
                c4::sliceResult remoteRevID(c4doc_getRemoteAncestor(doc, _remoteDBID));
                if (remoteRevID != revID)
                    updateRemoteRev(doc);
            }
            return true;
        }
        
        ancestors.resize(0);
        if (doc) {
            // Revision isn't found, but look for ancestors:
            if (c4doc_selectFirstPossibleAncestorOf(doc, revID)) {
                do {
                    ancestors.emplace_back(doc->selectedRev.revID);
                } while (c4doc_selectNextPossibleAncestorOf(doc, revID)
                         && ancestors.size() < kMaxPossibleAncestors);
            }
        } else if (!isNotFoundError(err)) {
            gotError(err);
        }
        return false;
    }


    // Updates the doc to have the currently-selected rev marked as the remote
    void DBWorker::updateRemoteRev(C4Document *doc) {
        slice revID = doc->selectedRev.revID;
        logVerbose("Updating remote #%u's rev of '%.*s' to %.*s",
                   _remoteDBID, SPLAT(doc->docID), SPLAT(revID));
        C4Error error;
        c4::Transaction t(_db);
        bool ok = t.begin(&error)
               && c4doc_setRemoteAncestor(doc, _remoteDBID, &error)
               && c4doc_save(doc, 0, &error)
               && t.commit(&error);
        if (!ok)
            warn("Failed to update remote #%u's rev of '%.*s' to %.*s: %d/%d",
                 _remoteDBID, SPLAT(doc->docID), SPLAT(revID), error.domain, error.code);
    }


    // Checks whether the revID (if any) is really current for the given doc.
    // Returns an HTTP-ish status code: 0=OK, 409=conflict, 500=internal error
    int DBWorker::findProposedChange(slice docID, slice revID, slice parentRevID) {
        C4Error err;
        //OPT: We don't need the document body, just its metadata, but there's no way to say that
        c4::ref<C4Document> doc = c4doc_get(_db, docID, true, &err);
        if (!doc) {
            if (isNotFoundError(err)) {
                // Doc doesn't exist; it's a conflict if the peer thinks it does:
                return parentRevID ? 409 : 0;
            } else {
                gotError(err);
                return 500;
            }
        } else if (slice(doc->revID) == revID) {
            // I already have this revision:
            return 304;
        } else if (!parentRevID) {
            // Peer is creating new doc; that's OK if doc is currently deleted:
            return (doc->flags & kDocDeleted) ? 0 : 409;
        } else if (slice(doc->revID) != parentRevID) {
            // Peer's revID isn't current, so this is a conflict:
            return 409;
        } else {
            // I don't have this revision and it's not a conflict, so I want it!
            return 0;
        }
    }


#pragma mark - SENDING REVISIONS:


    // Sends a document revision in a "rev" request.
    void DBWorker::_sendRevision(RevRequest request,
                                MessageProgressCallback onProgress)
    {
        if (!connection())
            return;
        logVerbose("Sending revision '%.*s' #%.*s",
                   SPLAT(request.docID), SPLAT(request.revID));
        C4Error c4err;
        slice revisionBody;
        C4RevisionFlags revisionFlags = 0;
        string history;
        Dict root;
        int blipError = 0;
        c4::ref<C4Document> doc = c4doc_get(_db, request.docID, true, &c4err);
        if (doc && c4doc_selectRevision(doc, request.revID, true, &c4err)) {
            revisionBody = slice(doc->selectedRev.body);
            if (revisionBody) {
                root = Value::fromTrustedData(revisionBody).asDict();
                if (!root) {
                    blipError = 500;
                    c4err = c4error_make(LiteCoreDomain, kC4ErrorCorruptData,
                                         "Unparseable revision body"_sl);
                } else if (root.count() == 0) {
                    root = Dict(); // no sense encoding an empty body later
                }
            }
            revisionFlags = doc->selectedRev.flags;

            // Generate the revision history string:
            set<pure_slice> ancestors(request.ancestorRevIDs.begin(), request.ancestorRevIDs.end());
            stringstream historyStream;
            for (int n = 0; n < request.maxHistory; ++n) {
                if (!c4doc_selectParentRevision(doc))
                    break;
                slice revID = doc->selectedRev.revID;
                if (n > 0)
                    historyStream << ',';
                historyStream << fleeceapi::asstring(revID);
                if (ancestors.find(revID) != ancestors.end())
                    break;
            }
            history = historyStream.str();
        } else {
            // Well, this is a pickle. I'm supposed to send a revision that I can't read.
            // I need to send a "rev" message, and I can't use a BLIP error response (because this
            // is a request not a response) so I'll add an "error" property to it instead of body.
            warn("sendRevision: Couldn't get '%.*s'/%.*s from db: %d/%d",
                 SPLAT(request.docID), SPLAT(request.revID), c4err.domain, c4err.code);
            doc = nullptr;
            if (c4err.domain == LiteCoreDomain && c4err.code == kC4ErrorNotFound)
                blipError = 404;
            else if (c4err.domain == LiteCoreDomain && c4err.code == kC4ErrorDeleted)
                blipError = 410;
            else
                blipError = 500;
        }

        // Now send the BLIP message:
        MessageBuilder msg("rev"_sl);
        msg.noreply = !onProgress;
        msg.compressed = true;
        msg["id"_sl] = request.docID;
        msg["rev"_sl] = request.revID;
        msg["sequence"_sl] = request.sequence;
        if (request.noConflicts)
            msg["noconflicts"_sl] = true;
        if (revisionFlags & kRevDeleted)
            msg["deleted"_sl] = "1"_sl;
        if (!history.empty())
            msg["history"_sl] = history;
        if (blipError)
            msg["error"_sl] = blipError;

        // Write doc body as JSON:
        if (root) {
            auto &bodyEncoder = msg.jsonBody();
            auto sk = c4db_getFLSharedKeys(_db);
            bodyEncoder.setSharedKeys(sk);
            if (request.legacyAttachments && (revisionFlags & kRevHasAttachments))
                writeRevWithLegacyAttachments(bodyEncoder, root, sk);
            else
                bodyEncoder.writeValue(root);
        } else {
            msg.write("{}"_sl);
        }
        sendRequest(msg, onProgress);
    }


    void DBWorker::writeRevWithLegacyAttachments(Encoder& enc, Dict root, FLSharedKeys sk) {
        enc.beginDict();

        // Write existing properties except for _attachments:
        Dict oldAttachments;
        for (Dict::iterator i(root, sk); i; ++i) {
            slice key = i.keyString();
            if (key == slice(kC4LegacyAttachmentsProperty)) {
                oldAttachments = i.value().asDict();    // remember _attachments dict for later
            } else {
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Now write _attachments:
        enc.writeKey("_attachments"_sl);
        enc.beginDict();
        // First pre-existing legacy attachments, if any:
        for (Dict::iterator i(oldAttachments, sk); i; ++i) {
            slice key = i.keyString();
            if (!key.hasPrefix("blob_"_sl)) {
                // TODO: Should skip this entry if a blob with the same digest exists
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Then entries for blobs found in the document:
        unsigned n = 0;
        IncomingRev::findBlobReferences(root, sk, [&](Dict dict, C4BlobKey blobKey) {
            char attName[32];
            sprintf(attName, "blob_%u", ++n);   // TODO: Mnemonic name based on JSON key path
            enc.writeKey(slice(attName));
            enc.beginDict();
            for (Dict::iterator i(dict, sk); i; ++i) {
                slice key = i.keyString();
                if (key != slice(kC4ObjectTypeProperty) && key != "stub"_sl) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.writeKey("stub"_sl);
            enc.writeBool(true);
            enc.writeKey("revpos"_sl);
            enc.writeInt(1);
            enc.endDict();
        });
        enc.endDict();

        enc.endDict();
    }


#pragma mark - INSERTING REVISIONS:


    void DBWorker::insertRevision(RevToInsert *rev) {
        lock_guard<mutex> lock(_revsToInsertMutex);
        if (!_revsToInsert) {
            _revsToInsert.reset(new vector<RevToInsert*>);
            _revsToInsert->reserve(500);
            enqueueAfter(kInsertionDelay, &DBWorker::_insertRevisionsNow);
        }
        _revsToInsert->push_back(rev);
    }


    void DBWorker::_insertRevisionsNow() {
        __typeof(_revsToInsert) revs;
        {
            lock_guard<mutex> lock(_revsToInsertMutex);
            revs = move(_revsToInsert);
            _revsToInsert.reset();
        }

        logVerbose("Inserting %zu revs:", revs->size());
        Stopwatch st;

        C4Error transactionErr;
        c4::Transaction transaction(_db);
        if (transaction.begin(&transactionErr)) {
            SharedEncoder enc(c4db_getSharedFleeceEncoder(_db));
            
            for (auto &rev : *revs) {
                // Add a revision:
                logVerbose("    {'%.*s' #%.*s}", SPLAT(rev->docID), SPLAT(rev->revID));
                vector<C4String> history;
                history.reserve(10);
                history.push_back(rev->revID);
                for (const void *pos=rev->historyBuf.buf, *end = rev->historyBuf.end(); pos < end;) {
                    auto comma = slice(pos, end).findByteOrEnd(',');
                    history.push_back(slice(pos, comma));
                    pos = comma + 1;
                }

                // rev->body is Fleece, but sadly we can't insert it directly because it doesn't
                // use the db's SharedKeys, so all of its Dict keys are strings. Putting this into
                // the db would cause failures looking up those keys (see #156). So re-encode:
                Value root = Value::fromTrustedData(rev->body);
                enc.writeValue(root);
                alloc_slice bodyForDB = enc.finish();
                enc.reset();
                rev->body = nullslice;

                C4DocPutRequest put = {};
                put.body = bodyForDB;
                put.docID = rev->docID;
                put.revFlags = rev->flags | kRevKeepBody;
                put.existingRevision = true;
                put.allowConflict = !rev->noConflicts;
                put.history = history.data();
                put.historyCount = history.size();
                put.remoteDBID = _remoteDBID;
                put.save = true;

                C4Error docErr;
                c4::ref<C4Document> doc = c4doc_put(_db, &put, nullptr, &docErr);
                if (!doc) {
                    warn("Failed to insert '%.*s' #%.*s : error %d/%d",
                         SPLAT(rev->docID), SPLAT(rev->revID), docErr.domain, docErr.code);
                    if (rev->onInserted)
                        rev->onInserted(docErr);
                    rev = nullptr;
                } else if (hasConflict(doc)) {
                    // Note that rev was inserted but caused a conflict:
                    log("Created conflict with '%.*s' #%.*s",
                        SPLAT(rev->docID), SPLAT(rev->revID));
                    rev->flags |= kRevIsConflict;
                }
            }
        }

        // Commit transaction:
        if (transaction.active() && transaction.commit(&transactionErr))
            transactionErr = { };
        else
            warn("Transaction failed!");

        // Notify all revs (that didn't already fail):
        for (auto rev : *revs) {
            if (rev && rev->onInserted)
                rev->onInserted(transactionErr);
        }

        if (transactionErr.code) {
            gotError(transactionErr);
        } else {
            double t = st.elapsed();
            log("Inserted %zu revs in %.2fms (%.0f/sec)", revs->size(), t*1000, revs->size()/t);
        }
    }

} }
