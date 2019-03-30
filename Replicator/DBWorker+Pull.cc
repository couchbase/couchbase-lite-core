//
// DBWorker+Pull.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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
#include "IncomingRev.hh"
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "Instrumentation.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "BLIP.hh"

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {


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
                    logDebug("    - Accepting proposed change '%.*s' #%.*s with parent %.*s",
                             SPLAT(docID), SPLAT(revID), SPLAT(parentRevID));
                    ++requested;
                    whichRequested[i] = true;
                } else {
                    logInfo("Rejecting proposed change '%.*s' #%.*s with parent %.*s (status %d; current rev is %.*s)",
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


#pragma mark - INSERTING & SYNCING REVISIONS:


    fleece::Doc DBWorker::tempEncodeJSON(slice jsonBody, FLError *err) {
        lock_guard<mutex> lock(_tempSKMutex);
        Encoder enc;
        enc.setSharedKeys(_tempSharedKeys);
        enc.convertJSON(jsonBody);
        Doc doc = enc.finishDoc();
        if (!doc && err)
            *err = enc.error();
        return doc;
    }


    static bool containsAttachmentsProperty(slice json) {
        if (!json.find("\"_attachments\":"_sl))
            return false;
        Doc doc = Doc::fromJSON(json);
        return doc.root().asDict()["_attachments"] != nullptr;
    }


    atomic<unsigned> DBWorker::gNumDeltasApplied;


    // Applies a delta to an existing revision.
    Doc DBWorker::_applyDelta(const C4Revision *baseRevision,
                              slice deltaJSON,
                              C4Error *outError)
    {
        Dict srcRoot = Value::fromData(baseRevision->body, kFLTrusted).asDict();
        if (!srcRoot) {
            if (outError) *outError = c4error_make(LiteCoreDomain, kC4ErrorCorruptRevisionData, nullslice);
            return {};
        }
        Doc legacy;
        if (!_disableBlobSupport && containsAttachmentsProperty(deltaJSON)) {
            // Delta refers to legacy attachments, so convert my base revision to have them:
            Encoder enc;
            writeRevWithLegacyAttachments(enc, srcRoot, 1);
            legacy = enc.finishDoc();
            srcRoot = legacy.root().asDict();
        }

        bool useDbSharedKeys = c4db_isInTransaction(_db);
        FLEncoder enc;
        if (useDbSharedKeys)
            enc = c4db_getSharedFleeceEncoder(_db);
        else {
            enc = FLEncoder_New();
            FLEncoder_SetSharedKeys(enc, _tempSharedKeys);
        }
        FLEncodeApplyingJSONDelta(srcRoot, deltaJSON, enc);
        ++gNumDeltasApplied;
        FLError flErr;
        Doc result = FLEncoder_FinishDoc(enc, &flErr);
        if (!result) {
            if (outError) {
                if (flErr == kFLInvalidData)
                    *outError = c4error_make(LiteCoreDomain, kC4ErrorCorruptDelta, "Invalid delta"_sl);
                else
                    *outError = {FleeceDomain, flErr};
            }
        }
        if (!useDbSharedKeys)
            FLEncoder_Free(enc);
        return result;
    }


    // Async version of _applyDelta -- called by IncomingRev
    void DBWorker::_applyDelta(Retained<RevToInsert> rev,
                               alloc_slice baseRevID,
                               alloc_slice deltaJSON,
                               std::function<void(Doc,C4Error)> callback)
    {
        Doc fleeceDoc;
        C4Error c4err;
        c4::ref<C4Document> doc = c4doc_get(_db, rev->docID, true, &c4err);
        if (doc && c4doc_selectRevision(doc, baseRevID, true, &c4err)) {
            if (doc->selectedRev.body.buf) {
                c4::Transaction t(_db);
                bool began = t.begin(&c4err);
                if(began) {
                    fleeceDoc = _applyDelta(&doc->selectedRev, deltaJSON, &c4err);
                    t.end(fleeceDoc != nullptr, &c4err);
                }
            } else {
                // Don't have the body of the source revision. This might be because I'm in
                // no-conflict mode and the peer is trying to push me a now-obsolete revision.
                if (_options.noIncomingConflicts()) {
                    c4err = {WebSocketDomain, 409};
                } else {
                    string msg = format("Couldn't apply delta: Don't have body of '%.*s' #%.*s [current is %.*s]",
                                        SPLAT(rev->docID), SPLAT(baseRevID), SPLAT(doc->revID));
                    warn("%s", msg.c_str());
                    c4err = c4error_make(LiteCoreDomain, kC4ErrorDeltaBaseUnknown, slice(msg));
                }
            }
        }
        callback(fleeceDoc, c4err);
    }


    // Callback from c4doc_put() that applies a delta, during _insertRevisionsNow()
    C4SliceResult DBWorker::applyDeltaCallback(const C4Revision *baseRevision,
                                               C4Slice deltaJSON,
                                               C4Error *outError)
    {
        Doc doc = _applyDelta(baseRevision, deltaJSON, outError);
        if (!doc)
            return {};
        alloc_slice body = doc.allocedData();
        if (!_disableBlobSupport) {
            // After applying the delta, remove legacy attachment properties and any other
            // "_"-prefixed top level properties:
            Dict root = doc.root().asDict();
            if (c4doc_hasOldMetaProperties(root)) {
                C4Error err;
                FLSharedKeys sk = c4db_getFLSharedKeys(_db);
                body = c4doc_encodeStrippingOldMetaProperties(root, sk, &err);
                if (!body) {
                    warn("Failed to strip legacy attachments: error %d/%d", err.domain, err.code);
                    if (outError)
                        *outError = c4error_make(WebSocketDomain, 500, "invalid legacy attachments"_sl);
                }
            }
        }
        return C4SliceResult(body);
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
            // Before updating docs, write all pending changes to remote ancestors, in case any
            // of them apply to the docs we're updating:
            _markRevsSyncedNow();

            SharedEncoder enc(c4db_getSharedFleeceEncoder(_db));
            lock_guard<mutex> lock(_tempSKMutex);   // bodies have been encoded with _tempSharedKeys

            for (RevToInsert *rev : *revs) {
                // Add a revision:
                vector<C4String> history;
                history.reserve(10);
                history.push_back(rev->revID);
                for (const void *pos=rev->historyBuf.buf, *end = rev->historyBuf.end(); pos < end;) {
                    auto comma = slice(pos, end).findByteOrEnd(',');
                    history.push_back(slice(pos, comma));
                    pos = comma + 1;
                }

                C4Error docErr;
                bool docSaved;

                if (rev->flags & kRevPurged) {
                    // Server says the document is no longer accessible, i.e. it's been
                    // removed from all channels the client has access to. Purge it.
                    docSaved = c4db_purgeDoc(_db, rev->docID, &docErr);
                    if (docSaved)
                        logVerbose("    {'%.*s' removed (purged)}", SPLAT(rev->docID));
                    else if (docErr.domain == LiteCoreDomain && docErr.code == kC4ErrorNotFound)
                        docSaved = true;
                } else {
                    // Set up the parameter block for c4doc_put():
                    C4DocPutRequest put = {};
                    put.docID = rev->docID;
                    put.revFlags = rev->flags;
                    put.existingRevision = true;
                    put.allowConflict = !rev->noConflicts;
                    put.history = history.data();
                    put.historyCount = history.size();
                    put.remoteDBID = _remoteDBID;
                    put.save = true;

                    alloc_slice bodyForDB;
                    if (rev->deltaSrcRevID) {
                        // If this is a delta, put the JSON delta in the body:
                        bodyForDB = move(rev->body);
                        put.deltaSourceRevID = rev->deltaSrcRevID;
                        put.deltaCB = [](void *context, const C4Revision *baseRev,
                                         C4Slice delta, C4Error *outError) {
                            return ((DBWorker*)context)->applyDeltaCallback(baseRev, delta, outError);
                        };
                        put.deltaCBContext = this;
                        // Preserve rev body as the source of a future delta I may push back:
                        put.revFlags |= kRevKeepBody;
                    } else {
                        // rev->body is Fleece, but sadly we can't insert it directly because it doesn't
                        // use the db's SharedKeys, so all of its Dict keys are strings. Putting this into
                        // the db would cause failures looking up those keys (see #156). So re-encode:
                        Value root = Value::fromData(rev->body, kFLTrusted);
                        enc.writeValue(root);
                        bodyForDB = enc.finish();
                        enc.reset();
                        rev->body = nullslice;
                        // Preserve rev body as the source of a future delta I may push back:
                        if (bodyForDB.size >= tuning::kMinBodySizeForDelta && !_disableDeltaSupport)
                            put.revFlags |= kRevKeepBody;
                    }
                    put.allocedBody = {(void*)bodyForDB.buf, bodyForDB.size};

                    // The save!!
                    c4::ref<C4Document> doc = c4doc_put(_db, &put, nullptr, &docErr);
                    if (doc) {
                        logVerbose("    {'%.*s' #%.*s <- %.*s} seq %llu",
                                   SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(rev->historyBuf),
                                   doc->selectedRev.sequence);
                        docSaved = true;
                        rev->sequence = doc->selectedRev.sequence;
                        if (doc->selectedRev.flags & kRevIsConflict) {
                            // Note that rev was inserted but caused a conflict:
                            logInfo("Created conflict with '%.*s' #%.*s",
                                SPLAT(rev->docID), SPLAT(rev->revID));
                            rev->flags |= kRevIsConflict;
                            rev->isWarning = true;
                            DebugAssert(put.allowConflict);
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
        }

        // Commit transaction:
        if (transaction.active() && transaction.commit(&transactionErr))
            transactionErr = { };
        else
            warn("Transaction failed!");

        // Notify all revs (that didn't already fail):
        for (auto rev : *revs) {
            if (rev->error.code == 0) {
                rev->error = transactionErr;
                if (rev->owner)
                    rev->owner->revisionInserted();
            }
        }

        if (transactionErr.code) {
            gotError(transactionErr);
        } else {
            double t = st.elapsed();
            logInfo("Inserted %zu revs in %.2fms (%.0f/sec)", revs->size(), t*1000, revs->size()/t);
        }
    }



} }
