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
#include "Address.hh"
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "SecureDigest.hh"
#include "c4.hh"
#include "BLIP.hh"
#include "RevID.hh"
#include <chrono>

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    static constexpr slice kPeerCheckpointStore  = "peerCheckpoints"_sl;

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
        _pushingDocs.clear();
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


    /*static*/ Dict DBWorker::getDocRoot(C4Document *doc, C4RevisionFlags *outFlags) {
        slice revisionBody(doc->selectedRev.body);
        if (!revisionBody)
            return nullptr;
        if (outFlags)
            *outFlags = doc->selectedRev.flags;
        return Value::fromData(revisionBody, kFLTrusted).asDict();
    }

    /*static*/ Dict DBWorker::getDocRoot(C4Document *doc, slice revID, C4RevisionFlags *outFlags) {
        if (c4doc_selectRevision(doc, revID, true, nullptr) && c4doc_loadRevisionBody(doc, nullptr))
            return getDocRoot(doc, outFlags);
        return nullptr;
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


    void DBWorker::findBlobReferences(Dict root, bool unique, const FindBlobCallback &callback) {
        // This method is non-static because it references _disableBlobSupport, but it's
        // thread-safe.
        set<string> found;
        FLDeepIterator i = FLDeepIterator_New(root);
        for (; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i)) {
            C4BlobKey blobKey;
            if (isAttachment(i, &blobKey, _disableBlobSupport)) {
                if (!unique || found.emplace((const char*)&blobKey, sizeof(blobKey)).second) {
                    auto blob = Value(FLDeepIterator_GetValue(i)).asDict();
                    callback(i, blob, blobKey);
                }
                FLDeepIterator_SkipChildren(i);
            }
        }
        FLDeepIterator_Free(i);
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

    
#pragma mark - MARK SYNCED


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
