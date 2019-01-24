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
#include "ReplicatorTuning.hh"
#include "Pusher.hh"
#include "IncomingRev.hh"
#include "Address.hh"
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "SecureDigest.hh"
#include "Stopwatch.hh"
#include "Instrumentation.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "BLIP.hh"
#include "RevID.hh"
#include <chrono>
#ifndef __APPLE__
#include "arc4random.h"
#endif

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    static constexpr slice kPeerCheckpointStore  = "peerCheckpoints"_sl;

    static bool isNotFoundError(C4Error err) {
        return err.domain == LiteCoreDomain && err.code == kC4ErrorNotFound;
    }

    DBWorker::DBWorker(Replicator *replicator,
                       C4Database *db,
                       const websocket::URL &remoteURL)
    :Worker(replicator, "DB")
    ,_db(c4db_retain(db))
    ,_blobStore(c4db_getBlobStore(db, nullptr))
    ,_remoteURL(remoteURL)
    ,_revsToInsert(this, &DBWorker::_insertRevisionsNow,
                   tuning::kInsertionDelay, tuning::kInsertionBatchSize)
    ,_revsToMarkSynced(this, &DBWorker::_markRevsSyncedNow, tuning::kInsertionDelay)
    {
        registerHandler("getCheckpoint",    &DBWorker::handleGetCheckpoint);
        registerHandler("setCheckpoint",    &DBWorker::handleSetCheckpoint);
        _disableBlobSupport = _options.properties["disable_blob_support"_sl].asBool();
        _disableDeltaSupport = _options.properties[kC4ReplicatorOptionDisableDeltas].asBool();
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
        return string(_remoteURL);
    }


    void DBWorker::_setCookie(alloc_slice setCookieHeader) {
        Address addr(_remoteURL);
        C4Error err;
        if (c4db_setCookie(_db, setCookieHeader,
                           addr.hostname, addr.path, &err)) {
            logVerbose("Set cookie: `%.*s`", SPLAT(setCookieHeader));
        } else {
            alloc_slice message = c4error_getDescription(err);
            warn("Unable to set cookie `%.*s`: %.*s",
                 SPLAT(setCookieHeader), SPLAT(message));
        }
    }


#pragma mark - CHECKPOINTS:


    alloc_slice DBWorker::_checkpointFromID(const slice &checkpointID, C4Error* err)
    {
        alloc_slice body;
        if (checkpointID) {
            const c4::ref<C4RawDocument> doc( c4raw_get(_db,
                                                        constants::kLocalCheckpointStore,
                                                        checkpointID,
                                                        err) );
            if (doc)
                body = alloc_slice(doc->body);
        }

        return body;
    }

    // Reads the local checkpoint & calls the callback; called by Replicator::getCheckpoints()
    void DBWorker::_getCheckpoint(CheckpointCallback callback) {
        C4Error err;
        alloc_slice checkpointID = alloc_slice(effectiveRemoteCheckpointDocID(&err));
        alloc_slice body = _checkpointFromID(checkpointID, &err);
        if(body.size == 0 && isNotFoundError(err)) {
            string oldCheckpointValue = _getOldCheckpoint(&err);
            if(oldCheckpointValue.empty()) {
                if(isNotFoundError(err)) {
                    err = {};
                }
            } else {
                checkpointID = alloc_slice(oldCheckpointValue);
                body = alloc_slice(_checkpointFromID(checkpointID, &err));
                if(body.size == 0) {
                    if(isNotFoundError(err)) {
                        err = {};
                    }
                }
            }
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

        const bool dbIsEmpty = c4db_getLastSequence(_db) == 0;
        callback(checkpointID, body, dbIsEmpty, err);
    }


    void DBWorker::_setCheckpoint(alloc_slice data, std::function<void()> onComplete) {
        C4Error err;
        const auto checkpointID = effectiveRemoteCheckpointDocID(&err);
        if (checkpointID && c4raw_put(_db, constants::kLocalCheckpointStore, checkpointID, nullslice, data, &err))
            logInfo("Saved local checkpoint %.*s to db", SPLAT(checkpointID));
        else
            gotError(err);
        onComplete();
    }

    string DBWorker::_getOldCheckpoint(C4Error* err)
    {
        const c4::ref<C4RawDocument> doc( c4raw_get(_db,
                                                  kC4InfoStore,
                                                  constants::kPreviousPrivateUUIDKey,
                                                  err) );
        if(!doc) {
            err->domain = LiteCoreDomain;
            err->code = kC4ErrorNotFound;
            return string();
        }

        C4UUID oldUUID = *(C4UUID*)doc->body.buf;
        return effectiveRemoteCheckpointDocID(&oldUUID, err);
    }
    
    // Writes a Value to an Encoder, substituting null if the value is an empty array.
    static void writeValueOrNull(fleece::Encoder &enc, Value val) {
        auto a = val.asArray();
        if (!val || (a && a.empty()))
            enc.writeNull();
        else
            enc.writeValue(val);
    }

    slice DBWorker::effectiveRemoteCheckpointDocID(C4Error* err)
    {
        if(_remoteCheckpointDocID.empty()) {
            C4UUID privateID;
            if(!c4db_getUUIDs(_db, nullptr, &privateID, err)) {
                return nullslice;
            }

            _remoteCheckpointDocID = effectiveRemoteCheckpointDocID(&privateID, err);
        }

        return slice(_remoteCheckpointDocID);
    }
    
    // Computes the ID of the checkpoint document.
    string DBWorker::effectiveRemoteCheckpointDocID(const C4UUID *localUUID, C4Error *err) {
        // Derive docID from from db UUID, remote URL, channels, filter, and docIDs.
        Array channels = _options.channels();
            Value filter = _options.properties[kC4ReplicatorOptionFilter];
        const Value filterParams = _options.properties[kC4ReplicatorOptionFilterParams];
        Array docIDs = _options.docIDs();

        // Compute the ID by writing the values to a Fleece array, then taking a SHA1 digest:
        fleece::Encoder enc;
        enc.beginArray();
        enc.writeString({localUUID, sizeof(C4UUID)});

        enc.writeString(remoteDBIDString());
        if (!channels.empty() || !docIDs.empty() || filter) {
            // Optional stuff:
            writeValueOrNull(enc, channels);
            writeValueOrNull(enc, filter);
            writeValueOrNull(enc, filterParams);
            writeValueOrNull(enc, docIDs);
        }
        enc.endArray();
        const alloc_slice data = enc.finish();
        SHA1 digest(data);
        string finalProduct = string("cp-") + slice(&digest, sizeof(digest)).base64String();
        logVerbose("Checkpoint doc ID = %s", finalProduct.c_str());
        return finalProduct;
    }


    bool DBWorker::getPeerCheckpointDoc(MessageIn* request, bool getting,
                                       slice &checkpointID, c4::ref<C4RawDocument> &doc) const {
        checkpointID = request->property("client"_sl);
        if (!checkpointID) {
            request->respondWithError({"BLIP"_sl, 400, "missing checkpoint ID"_sl});
            return false;
        }
        logInfo("Request to %s checkpoint '%.*s'",
            (getting ? "get" : "set"), SPLAT(checkpointID));

        C4Error err;
        doc = c4raw_get(_db, kPeerCheckpointStore, checkpointID, &err);
        if (!doc) {
            const int status = isNotFoundError(err) ? 404 : 502;
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
            try {
                revid parsedRev(actualRev);
                generation = parsedRev.generation();
            } catch(error &e) {
                if(e.domain == error::Domain::LiteCore && e.code == error::LiteCoreError::CorruptRevisionData) {
                    actualRev = nullslice;
                } else {
                    throw;
                }
            }
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


    void DBWorker::_checkpointIsInvalid() {
        _checkpointValid = false;
    }


#pragma mark - CHANGES:


    static Dict getDocRoot(C4Document *doc) {
        slice revisionBody(doc->selectedRev.body);
        if (!revisionBody)
            return nullptr;
        return Value::fromData(revisionBody, kFLTrusted).asDict();
    }

    static Dict getDocRoot(C4Document *doc, slice revID) {
        if (c4doc_selectRevision(doc, revID, true, nullptr) && c4doc_loadRevisionBody(doc, nullptr))
            return getDocRoot(doc);
        return nullptr;
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
        logVerbose("Reading up to %u local changes since #%llu", p.limit, p.since);
        _getForeignAncestors = p.getForeignAncestors;
        _skipForeignChanges = p.skipForeign;
        _pushDocIDs = p.docIDs;
        if (_maxPushedSequence == 0)
            _maxPushedSequence = p.since;

        if (_getForeignAncestors)
            _markRevsSyncedNow();   // make sure foreign ancestors are up to date

        // Run a by-sequence enumerator to find the changed docs:
        auto changes = make_shared<RevToSendList>();
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (!p.getForeignAncestors && !_options.pushFilter)
            options.flags &= ~kC4IncludeBodies;
        if (!p.skipDeleted)
            options.flags |= kC4IncludeDeleted;
        c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(_db, p.since, &options, &error);
        if (e) {
            changes->reserve(p.limit);
            while (c4enum_next(e, &error) && p.limit > 0) {
                C4DocumentInfo info;
                c4enum_getDocumentInfo(e, &info);
                _maxPushedSequence = info.sequence;
                auto rev = retained(new RevToSend(info));
                if (addChangeToList(rev, e, changes))
                    --p.limit;
            }
        }

        _pusher = pusher;
        pusher->gotChanges(move(changes), _maxPushedSequence, error);

        if (p.continuous && p.limit > 0 && !_changeObserver) {
            // Reached the end of history; now start observing for future changes
            _changeObserver = c4dbobs_create(_db,
                                             [](C4DatabaseObserver* observer, void *context) {
                                                 auto self = (DBWorker*)context;
                                                 self->enqueue(&DBWorker::dbChanged);
                                             },
                                             this);
            logDebug("Started DB observer");
        }
    }


    // (Async) callback from the C4DatabaseObserver when the database has changed
    void DBWorker::dbChanged() {
        if (!_changeObserver)
            return; // if replication has stopped already by the time this async call occurs

        if (_getForeignAncestors)
            _markRevsSyncedNow();   // make sure foreign ancestors are up to date

        static const uint32_t kMaxChanges = 100;
        C4DatabaseChange c4changes[kMaxChanges];
        bool external;
        uint32_t nChanges;
        shared_ptr<RevToSendList> changes;

        while (true) {
            nChanges = c4dbobs_getChanges(_changeObserver, c4changes, kMaxChanges, &external);
            if (nChanges == 0)
                break;        // no more changes
            if (!external) {
                logDebug("Notified of %u of my own db changes #%llu ... #%llu (ignoring)",
                         nChanges, c4changes[0].sequence, c4changes[nChanges-1].sequence);
                _maxPushedSequence = c4changes[nChanges-1].sequence;
                continue;     // ignore changes I made myself
            }
            logVerbose("Notified of %u db changes #%llu ... #%llu",
                       nChanges, c4changes[0].sequence, c4changes[nChanges-1].sequence);

            // Copy the changes into a vector of RevToSend:
            C4DatabaseChange *c4change = c4changes;
            for (uint32_t i = 0; i < nChanges; ++i, ++c4change) {
                if (!changes) {
                    changes = make_shared<RevToSendList>();
                    changes->reserve(nChanges - i);
                }
                _maxPushedSequence = c4change->sequence;
                auto rev = retained(new RevToSend({0, c4change->docID, c4change->revID,
                                                   c4change->sequence, c4change->bodySize}));
                // Note: we send tombstones even if the original getChanges() call specified
                // skipDeletions. This is intentional; skipDeletions applies only to the initial
                // dump of existing docs, not to 'live' changes.
                if (addChangeToList(rev, nullptr, changes) && changes->size() >= kMaxChanges) {
                    _pusher->gotChanges(move(changes), _maxPushedSequence, {});
                    changes.reset();
                }
            }

            c4dbobs_releaseChanges(c4changes, nChanges);
        }

        if (changes && changes->size() > 0)
            _pusher->gotChanges(move(changes), _maxPushedSequence, {});
    }


    // Common subroutine of _getChanges and dbChanged that adds a document to a list of Revs.
    bool DBWorker::addChangeToList(RevToSend *rev,
                                   C4DocEnumerator *e,
                                   shared_ptr<RevToSendList> &changes)
    {
        if (_pushDocIDs != nullptr)
            if (_pushDocIDs->find(slice(rev->docID).asString()) == _pushDocIDs->end())
                return false;

        // _pushingDocs has an entry for each docID involved in the push process, from change
        // detection all the way to confirmation of the upload. The value of the entry is usually
        // null; if not, it holds a later revision of that document that should be processed
        // after the current one is done.
        auto active = _pushingDocs.find(rev->docID);
        if (active != _pushingDocs.end()) {
            // This doc already has a revision being sent; wait till that one is done
            logDebug("Holding off on change '%.*s' %.*s till earlier rev is done",
                     SPLAT(rev->docID), SPLAT(rev->revID));
            active->second = rev;
            return false;
        }

        if (rev->expiration > 0 && rev->expiration < c4_now()) {
            logVerbose("'%.*s' is expired; not pushing it", SPLAT(rev->docID));
            return false;
        }

        bool needRemoteRevID = (_getForeignAncestors && _checkpointValid);
        if (needRemoteRevID || _options.pushFilter) {
            c4::ref<C4Document> doc;
            C4Error error;
            doc = e ? c4enum_getDocument(e, &error) : c4doc_get(_db, rev->docID, true, &error);
            if (!doc) {
                finishedDocumentWithError(rev, error, false);
                return false;   // reject rev: error getting doc
            }
            if (slice(doc->revID) != slice(rev->revID))
                return false;   // ignore rev: there's a newer one already

            if (needRemoteRevID) {
                // For proposeChanges, find the nearest foreign ancestor of the current rev:
                Assert(_remoteDBID);
                alloc_slice foreignAncestor( c4doc_getRemoteAncestor(doc, _remoteDBID) );
                logDebug("remoteRevID of '%.*s' is %.*s", SPLAT(doc->docID), SPLAT(foreignAncestor));
                if (_skipForeignChanges && foreignAncestor == slice(rev->revID))
                    return false;   // skip this rev: it's already on the peer
                rev->remoteAncestorRevID = alloc_slice(foreignAncestor);
            }

            if (_options.pushFilter) {
                if (!_options.pushFilter(doc->docID, doc->selectedRev.flags,
                                         getDocRoot(doc), _options.callbackContext)) {
                    logVerbose("Doc '%.*s' rejected by push filter", SPLAT(doc->docID));
                    return false;
                }
            }
        }

        _pushingDocs.insert({rev->docID, nullptr});
        changes->push_back(rev);
        return true;
    }


    // Called by the Puller; handles a "changes" or "proposeChanges" message by checking which of
    // the changes don't exist locally, and returning a bit-vector indicating them.
    void DBWorker::_findOrRequestRevs(Retained<MessageIn> req,
                                     function<void(vector<bool>)> callback) {
        Signpost signpost(Signpost::get);
        // Iterate over the array in the message, seeing whether I have each revision:
        bool proposed = (req->property("Profile"_sl) == "proposeChanges"_sl);
        auto changes = req->JSONBody().asArray();
        if (willLog() && !changes.empty()) {
            if (proposed) {
                logInfo("Received %u changes", changes.count());
            } else {
                alloc_slice firstSeq(changes[0].asArray()[0].toString());
                alloc_slice lastSeq (changes[changes.count()-1].asArray()[0].toString());
                logInfo("Received %u changes (seq '%.*s'..'%.*s')",
                    changes.count(), SPLAT(firstSeq), SPLAT(lastSeq));
            }
        }

        if (!proposed)
            _markRevsSyncedNow();   // make sure foreign ancestors are up to date

        MessageBuilder response(req);
        response.compressed = true;
        response["maxHistory"_sl] = c4db_getMaxRevTreeDepth(_db);
        if (!_disableBlobSupport)
            response["blobs"_sl] = "true"_sl;
        if (!_disableDeltaSupport && !_announcedDeltaSupport) {
            response["deltas"_sl] = "true"_sl;
            _announcedDeltaSupport = true;
        }
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
                alloc_slice currentRevID;
                int status = findProposedChange(docID, revID, parentRevID, currentRevID);
                if (status == 0) {
                    ++requested;
                    whichRequested[i] = true;
                } else {
                    logInfo("Rejecting proposed change '%.*s' %.*s with parent %.*s (status %d; current rev is %.*s)",
                        SPLAT(docID), SPLAT(revID), SPLAT(parentRevID), status, SPLAT(currentRevID));
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

        req->respond(response);
        logInfo("Responded to '%.*s' REQ#%llu w/request for %u revs",
            SPLAT(req->property("Profile"_sl)), req->number(), requested);
    }


    // Returns true if revision exists; else returns false and sets ancestors to an array of
    // ancestor revisions I do have (empty if doc doesn't exist at all)
    bool DBWorker::findAncestors(slice docID, slice revID, vector<alloc_slice> &ancestors) {
        C4Error err;
        c4::ref<C4Document> doc = c4doc_get(_db, docID, true, &err);
        if (!doc) {
            ancestors.resize(0);
            if (!isNotFoundError(err))
                gotError(err);
            return false;
        }

        alloc_slice remoteRevID;
        if (_remoteDBID)
            remoteRevID = c4doc_getRemoteAncestor(doc, _remoteDBID);

        if (c4doc_selectRevision(doc, revID, false, &err)) {
            // I already have this revision. Make sure it's marked as current for this remote:
            if (remoteRevID != revID && _remoteDBID)
                updateRemoteRev(doc);
            return true;
        }

        auto addAncestor = [&]() {
            if (_disableDeltaSupport || c4doc_hasRevisionBody(doc))  // need body for deltas
                ancestors.emplace_back(doc->selectedRev.revID);
        };

        // Revision isn't found, but look for ancestors. Start with the common ancestor:
        ancestors.resize(0);
        if (c4doc_selectRevision(doc, remoteRevID, true, &err))
            addAncestor();

        if (c4doc_selectFirstPossibleAncestorOf(doc, revID)) {
            do {
                if (doc->selectedRev.revID != remoteRevID)
                    addAncestor();
            } while (c4doc_selectNextPossibleAncestorOf(doc, revID)
                     && ancestors.size() < kMaxPossibleAncestors);
        }
        return false;
    }


    // Updates the doc to have the currently-selected rev marked as the remote
    void DBWorker::updateRemoteRev(C4Document *doc) {
        slice revID = doc->selectedRev.revID;
        logInfo("Updating remote #%u's rev of '%.*s' to %.*s",
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
    int DBWorker::findProposedChange(slice docID, slice revID, slice parentRevID,
                                     alloc_slice &outCurrentRevID)
    {
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
        }
        int status;
        if (slice(doc->revID) == revID) {
            // I already have this revision:
            status = 304;
        } else if (!parentRevID) {
            // Peer is creating new doc; that's OK if doc is currently deleted:
            status = (doc->flags & kDocDeleted) ? 0 : 409;
        } else if (slice(doc->revID) != parentRevID) {
            // Peer's revID isn't current, so this is a conflict:
            status = 409;
        } else {
            // I don't have this revision and it's not a conflict, so I want it!
            status = 0;
        }
        if (status > 0)
            outCurrentRevID = slice(doc->revID);
        return status;
    }


#pragma mark - SENDING REVISIONS:


    // Sends a document revision in a "rev" request.
    void DBWorker::_sendRevision(Retained<RevToSend> request, MessageProgressCallback onProgress) {
        if (!connection())
            return;
        logVerbose("Reading document '%.*s' #%.*s",
                   SPLAT(request->docID), SPLAT(request->revID));

        // Get the document & revision:
        C4Error c4err;
        slice revisionBody;
        Dict root;
        c4::ref<C4Document> doc = c4doc_get(_db, request->docID, true, &c4err);
        if (doc) {
            revisionBody = getRevToSend(doc, *request, &c4err);
            if (revisionBody) {
                root = Value::fromData(revisionBody, kFLTrusted).asDict();
                if (!root)
                    c4err = {LiteCoreDomain, kC4ErrorCorruptData};
                request->flags = doc->selectedRev.flags;
            }
        }

        // Now send the BLIP message. Normally it's "rev", but if this is an error we make it
        // "norev" and include the error code:
        MessageBuilder msg(root ? "rev"_sl : "norev"_sl);
        msg.compressed = true;
        msg["id"_sl] = request->docID;
        msg["rev"_sl] = request->revID;
        msg["sequence"_sl] = request->sequence;
        if (root) {
            auto sk = c4db_getFLSharedKeys(_db);
            auto revisionFlags = doc->selectedRev.flags;
            bool sendLegacyAttachments = (request->legacyAttachments
                                          && (revisionFlags & kRevHasAttachments)
                                          && !_disableBlobSupport);
            alloc_slice delta;
            if (request->deltaOK && !sendLegacyAttachments
                                 && !_disableDeltaSupport
                                 && revisionBody.size >= tuning::kMinBodySizeForDelta) {
                // Delta-encode:
                Dict ancestor = getDeltaSourceRev(doc, *request);
                if (ancestor) {
                    delta = FLCreateJSONDelta(ancestor, root);
                    if (!delta)
                        delta = "{}"_sl;
                    else if (delta.size > revisionBody.size * 1.2)
                        delta = nullslice;       // Delta is (probably) bigger than body; don't use
                }
                if (delta) {
                    msg["deltaSrc"_sl] = request->remoteAncestorRevID;
                    if (willLog(LogLevel::Verbose)) {
                        alloc_slice old (ancestor.toJSON(sk));
                        alloc_slice nuu (root.toJSON(sk));
                        logVerbose("Encoded revision as delta, saving %zd bytes:\n\told = %.*s\n\tnew = %.*s\n\tDelta = %.*s",
                                   nuu.size - delta.size,
                                   SPLAT(old), SPLAT(nuu), SPLAT(delta));
                    }
                }
            }

            msg.noreply = !onProgress;
            if (request->noConflicts)
                msg["noconflicts"_sl] = true;
            if (revisionFlags & kRevDeleted)
                msg["deleted"_sl] = "1"_sl;
            string history = revHistoryString(doc, *request);
            if (!history.empty())
                msg["history"_sl] = history;

            // Write doc body (or delta) as JSON:
                auto &bodyEncoder = msg.jsonBody();
            if (delta) {
                bodyEncoder.writeRaw(delta);
            } else if (root.empty()) {
                msg.write("{}"_sl);
            } else {
                if (sendLegacyAttachments)
                    writeRevWithLegacyAttachments(bodyEncoder, root,
                                                  c4rev_getGeneration(request->revID));
                else
                    bodyEncoder.writeValue(root);
            }
            logVerbose("Transmitting 'rev' message with '%.*s' #%.*s",
                       SPLAT(request->docID), SPLAT(request->revID));
            sendRequest(msg, onProgress);

        } else {
            // Send an error if we couldn't get the revision:
            int blipError;
            if (c4err.domain == WebSocketDomain)
                blipError = c4err.code;
            else if (c4err.domain == LiteCoreDomain && c4err.code == kC4ErrorNotFound)
                blipError = 404;
            else {
                warn("sendRevision: Couldn't get rev '%.*s' %.*s from db: %d/%d",
                     SPLAT(request->docID), SPLAT(request->revID), c4err.domain, c4err.code);
                blipError = 500;
            }
            msg["error"_sl] = blipError;
            msg.noreply = true;
            sendRequest(msg);
            // invoke the progress callback with a fake disconnect so the Pusher will know the
            // rev failed to send:
            if (onProgress)
                _pusher->couldntSendRevision(request);
        }
    }


    slice DBWorker::getRevToSend(C4Document* doc, const RevToSend &request, C4Error *c4err) {
        if (!c4doc_selectRevision(doc, request.revID, true, c4err))
            return nullslice;

        slice revisionBody(doc->selectedRev.body);
        if (!revisionBody) {
            logInfo("Revision '%.*s' #%.*s is obsolete; not sending it",
                SPLAT(request.docID), SPLAT(request.revID));
            *c4err = {WebSocketDomain, 410}; // Gone
        }
        return revisionBody;
    }


    // Returns the source of the delta to send for a given RevToSend
    Dict DBWorker::getDeltaSourceRev(C4Document* doc, const RevToSend &request) {
        if (request.remoteAncestorRevID) {
            Dict src = getDocRoot(doc, request.remoteAncestorRevID);
            if (src)
                return src;
        }
        if (request.ancestorRevIDs) {
            for (auto revID : *request.ancestorRevIDs) {
                Dict src = getDocRoot(doc, revID);
                if (src)
                    return src;
            }
        }
        return nullptr;
    }


    string DBWorker::revHistoryString(C4Document *doc, const RevToSend &request) {
        Assert(c4doc_selectRevision(doc, request.revID, true, nullptr));
        stringstream historyStream;
        int nWritten = 0;
        unsigned lastGen = c4rev_getGeneration(doc->selectedRev.revID);
        for (int n = 0; n < request.maxHistory; ++n) {
            if (!c4doc_selectParentRevision(doc))
                break;
            slice revID = doc->selectedRev.revID;
            unsigned gen = c4rev_getGeneration(revID);
            while (gen < --lastGen) {
                char fakeID[50];
                sprintf(fakeID, "%u-faded000%.08x%.08x", lastGen, arc4random(), arc4random());
                if (nWritten++ > 0)
                    historyStream << ',';
                historyStream << fakeID;
            }
            if (nWritten++ > 0)
                historyStream << ',';
            historyStream << revID.asString();
            if (request.hasRemoteAncestor(revID))
                break;
        }
        return historyStream.str();
    }


    void DBWorker::writeRevWithLegacyAttachments(fleece::Encoder& enc, Dict root,
                                                 unsigned revpos) {
        enc.beginDict();

        // Write existing properties except for _attachments:
        Dict oldAttachments;
        for (Dict::iterator i(root); i; ++i) {
            slice key = i.keyString();
            if (key == slice(kC4LegacyAttachmentsProperty)) {
                oldAttachments = i.value().asDict();    // remember _attachments dict for later
            } else {
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Now write _attachments:
        enc.writeKey(slice(kC4LegacyAttachmentsProperty));
        enc.beginDict();
        // First pre-existing legacy attachments, if any:
        for (Dict::iterator i(oldAttachments); i; ++i) {
            slice key = i.keyString();
            if (!key.hasPrefix("blob_"_sl)) {
                // TODO: Should skip this entry if a blob with the same digest exists
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Then entries for blobs found in the document:
        findBlobReferences(root, [&](FLDeepIterator di, FLDict blob, C4BlobKey blobKey) {
            alloc_slice path(FLDeepIterator_GetJSONPointer(di));
            string attName = string("blob_") + string(path);
            enc.writeKey(slice(attName));
            enc.beginDict();
            for (Dict::iterator i(blob); i; ++i) {
                slice key = i.keyString();
                if (key != slice(kC4ObjectTypeProperty) && key != "stub"_sl) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.writeKey("stub"_sl);
            enc.writeBool(true);
            enc.writeKey("revpos"_sl);
            enc.writeInt(revpos);
            enc.endDict();
        });
        enc.endDict();

        enc.endDict();
    }


    static inline bool isAttachment(FLDeepIterator i, C4BlobKey *blobKey, bool noBlobs) {
        auto dict = FLValue_AsDict(FLDeepIterator_GetValue(i));
        if (!dict)
            return false;
        if (!noBlobs && c4doc_dictIsBlob(dict, blobKey))
            return true;
        FLPathComponent* path;
        size_t depth;
        FLDeepIterator_GetPath(i, &path, &depth);
        return depth == 2
            && FLSlice_Equal(path[0].key, FLSTR(kC4LegacyAttachmentsProperty))
            && c4doc_getDictBlobKey(dict, blobKey);
    }


    void DBWorker::findBlobReferences(Dict root, const FindBlobCallback &callback) {
        // This method is non-static because it references _disableBlobSupport, but it's
        // thread-safe.
        set<string> found;
        FLDeepIterator i = FLDeepIterator_New(root);
        for (; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i)) {
            C4BlobKey blobKey;
            if (isAttachment(i, &blobKey, _disableBlobSupport)) {
                if (found.emplace((const char*)&blobKey, sizeof(blobKey)).second) {
                    auto blob = Value(FLDeepIterator_GetValue(i)).asDict();
                    callback(i, blob, blobKey);
                }
                FLDeepIterator_SkipChildren(i);
            }
        }
        FLDeepIterator_Free(i);
    }


#pragma mark - INSERTING & SYNCING REVISIONS:


    // Applies a delta, asynchronously returning expanded Fleece -- called by IncomingRev
    void DBWorker::_applyDelta(alloc_slice docID, alloc_slice baseRevID,
                               alloc_slice deltaJSON,
                               std::function<void(alloc_slice body, C4Error)> callback)
    {
        alloc_slice body;
        C4Error c4err;
        c4::ref<C4Document> doc = c4doc_get(_db, docID, true, &c4err);
        if (doc && c4doc_selectRevision(doc, baseRevID, true, &c4err)) {
            Dict srcRoot = getDocRoot(doc);
            if (srcRoot) {
                FLError flErr;
                body = FLApplyJSONDelta(srcRoot, deltaJSON, &flErr);
                if (!body)
                    c4err = {FleeceDomain, flErr};
            } else {
                c4err = {LiteCoreDomain, kC4ErrorCorruptRevisionData};
            }
        }
        callback(body, c4err);
    }


    void DBWorker::insertRevision(RevToInsert *rev) {
        _revsToInsert.push(rev);
    }


    // Insert all the revisions queued for insertion, and sync the ones queued for syncing.
    void DBWorker::_insertRevisionsNow() {
        auto revs = _revsToInsert.pop();
        if (!revs)
            return;

        logVerbose("Inserting %zu revs:", revs->size());
        Stopwatch st;

        C4Error transactionErr;
        c4::Transaction transaction(_db);
        if (transaction.begin(&transactionErr)) {
            SharedEncoder enc(c4db_getSharedFleeceEncoder(_db));
            
            for (RevToInsert *rev : *revs) {
                // Add a revision:
                logVerbose("    {'%.*s' #%.*s <- %.*s}", SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(rev->historyBuf));
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
                Value root = Value::fromData(rev->body, kFLTrusted);
                C4Error docErr;
                bool docSaved;

                if (rev->flags & kRevPurged) {
                    // Server says the document is no longer accessible, i.e. it's been
                    // removed from all channels the client has access to. Purge it.
                    docSaved = c4db_purgeDoc(_db, rev->docID, &docErr);
                    if (!docSaved && docErr.domain == LiteCoreDomain && docErr.code == kC4ErrorNotFound)
                        docSaved = true;
                } else {
                    enc.writeValue(root);
                    alloc_slice bodyForDB = enc.finish();
                    enc.reset();
                    rev->body = nullslice;

                    C4DocPutRequest put = {};
                    put.allocedBody = {(void*)bodyForDB.buf, bodyForDB.size};
                    put.docID = rev->docID;
                    put.revFlags = rev->flags;
                    put.existingRevision = true;
                    put.allowConflict = !rev->noConflicts;
                    put.history = history.data();
                    put.historyCount = history.size();
                    put.remoteDBID = _remoteDBID;
                    put.save = true;

                    c4::ref<C4Document> doc = c4doc_put(_db, &put, nullptr, &docErr);
                    if (doc) {
                        docSaved = true;
                        rev->sequence = doc->selectedRev.sequence;
                        if (doc->selectedRev.flags & kRevIsConflict) {
                            // Note that rev was inserted but caused a conflict:
                            logInfo("Created conflict with '%.*s' #%.*s",
                                SPLAT(rev->docID), SPLAT(rev->revID));
                            rev->flags |= kRevIsConflict;
                            rev->isWarning = true;
                        }
                    } else {
                        docSaved = false;
                    }
                }

                if (!docSaved) {
                    alloc_slice desc = c4error_getDescription(docErr);
                    warn("Failed to insert '%.*s' #%.*s : %.*s",
                         SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(desc));
                    rev->error = docErr;
                    if (rev->owner)
                        rev->owner->revisionInserted();
                }
            }

            // Also mark revs as synced, if any, while still in the transaction:
            _markRevsSyncedNow();
        }

        // Commit transaction:
        if (transaction.active() && transaction.commit(&transactionErr))
            transactionErr = { };
        else
            warn("Transaction failed!");

        // Notify all revs (that didn't already fail):
        for (auto rev : *revs) {
            rev->error = transactionErr;
            if (rev->owner)
                rev->owner->revisionInserted();
        }

        if (transactionErr.code) {
            gotError(transactionErr);
        } else {
            double t = st.elapsed();
            logInfo("Inserted %zu revs in %.2fms (%.0f/sec)", revs->size(), t*1000, revs->size()/t);
        }
    }


    void DBWorker::_donePushingRev(RetainedConst<RevToSend> rev, bool completed) {
        if (completed && _options.push > kC4Passive)
            _revsToMarkSynced.push((RevToSend*)rev.get());

        auto i = _pushingDocs.find(rev->docID);
        if (i == _pushingDocs.end()) {
            warn("_donePushingRev('%.*s'): That docID is not active!", SPLAT(rev->docID));
            return;
        }
        Retained<RevToSend> newRev = i->second;
        _pushingDocs.erase(i);
        if (newRev) {
            if (completed)
                newRev->remoteAncestorRevID = rev->revID;
            logDebug("Now that '%.*s' %.*s is done, propose %.*s (parent %.*s) ...",
                     SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(newRev->revID),
                     SPLAT(newRev->remoteAncestorRevID));
            auto changes = make_shared<RevToSendList>();
            if (addChangeToList(newRev, nullptr, changes)) {
                _maxPushedSequence = max(_maxPushedSequence, rev->sequence);
                _pusher->gotChanges(move(changes), _maxPushedSequence, {});
            } else {
                logDebug("   ... nope, decided not to propose '%.*s' %.*s",
                         SPLAT(newRev->docID), SPLAT(newRev->revID));
            }
        } else {
            logDebug("Done pushing '%.*s' %.*s", SPLAT(rev->docID), SPLAT(rev->revID));
        }
    }


    // Mark this revision as synced (i.e. the server's current revision) soon.
    // NOTE: While this is queued, calls to c4doc_getRemoteAncestor() for this document won't
    // return the correct answer, because the change hasn't been made in the database yet.
    // For that reason, this class ensures that _markRevsSyncedNow() is called before any call
    // to c4doc_getRemoteAncestor().
    void DBWorker::markRevSynced(ReplicatedRev *rev) {
        _revsToMarkSynced.push(rev);
    }


    // Mark all the queued revisions as synced to the server.
    void DBWorker::_markRevsSyncedNow() {
        auto revs = _revsToMarkSynced.pop();
        if (!revs)
            return;

        Stopwatch st;
        C4Error error;
        c4::Transaction transaction(_db);
        if (transaction.begin(&error)) {
            for (ReplicatedRev *rev : *revs) {
                logDebug("Marking rev '%.*s' %.*s (#%llu) as synced to remote db %u",
                         SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence, _remoteDBID);
                if (!c4db_markSynced(_db, rev->docID, rev->sequence, _remoteDBID, &error))
                    warn("Unable to mark '%.*s' %.*s (#%llu) as synced; error %d/%d",
                         SPLAT(rev->docID), SPLAT(rev->revID), rev->sequence, error.domain, error.code);
            }
            if (transaction.commit(&error)) {
                double t = st.elapsed();
                logInfo("Marked %zu revs as synced-to-server in %.2fms (%.0f/sec)",
                    revs->size(), t*1000, revs->size()/t);
                return;
            }
        }
        warn("Error marking %zu revs as synced: %d/%d", revs->size(), error.domain, error.code);
    }


#pragma mark - PROGRESS / ACTIVITY LEVEL:


    Worker::ActivityLevel DBWorker::computeActivityLevel() const {
        ActivityLevel level = Worker::computeActivityLevel();
        if (!_pushingDocs.empty())
            level = kC4Busy;
        if (SyncBusyLog.effectiveLevel() <= LogLevel::Info) {
            logInfo("activityLevel=%-s: pendingResponseCount=%d, eventCount=%d, activeDocs=%zu",
                    kC4ReplicatorActivityLevelNames[level],
                    pendingResponseCount(), eventCount(),
                    _pushingDocs.size());
        }
        return level;
    }

} }
