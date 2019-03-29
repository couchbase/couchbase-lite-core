//
// Puller+DB.cc
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

#include "Puller.hh"
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
    vector<bool> Puller::findOrRequestRevs(Retained<MessageIn> req) {
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
            _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date

        MessageBuilder response(req);
        response.compressed = true;
        _db->use([&](C4Database *db) {
            response["maxHistory"_sl] = c4db_getMaxRevTreeDepth(db);
        });
        if (!_db->disableBlobSupport())
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

        req->respond(response);
        logInfo("Responded to '%.*s' REQ#%llu w/request for %u revs",
            SPLAT(req->property("Profile"_sl)), req->number(), requested);

        return whichRequested;
    }


    // Checks whether the revID (if any) is really current for the given doc.
    // Returns an HTTP-ish status code: 0=OK, 409=conflict, 500=internal error
    int Puller::findProposedChange(slice docID, slice revID, slice parentRevID,
                                     alloc_slice &outCurrentRevID)
    {
        C4Error err;
        //OPT: We don't need the document body, just its metadata, but there's no way to say that
        c4::ref<C4Document> doc = _db->getDoc(docID, &err);
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
    bool Puller::findAncestors(slice docID, slice revID, vector<alloc_slice> &ancestors) {
        C4Error err;
        c4::ref<C4Document> doc = _db->getDoc(docID, &err);
        if (!doc) {
            ancestors.resize(0);
            if (!isNotFoundError(err))
                gotError(err);
            return false;
        }

        alloc_slice remoteRevID = _db->getDocRemoteAncestor(doc);

        if (c4doc_selectRevision(doc, revID, false, &err)) {
            // I already have this revision. Make sure it's marked as current for this remote:
            if (remoteRevID != revID && _db->remoteDBID())
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
    void Puller::updateRemoteRev(C4Document *doc) {
        slice revID = doc->selectedRev.revID;
        logInfo("Updating remote #%u's rev of '%.*s' to %.*s",
                   _db->remoteDBID(), SPLAT(doc->docID), SPLAT(revID));
        C4Error error;
        bool ok = _db->use<bool>([&](C4Database *db) {
            c4::Transaction t(db);
            return t.begin(&error)
                   && c4doc_setRemoteAncestor(doc, _db->remoteDBID(), &error)
                   && c4doc_save(doc, 0, &error)
                   && t.commit(&error);
        });
        if (!ok)
            warn("Failed to update remote #%u's rev of '%.*s' to %.*s: %d/%d",
                 _db->remoteDBID(), SPLAT(doc->docID), SPLAT(revID), error.domain, error.code);
    }


#pragma mark - DELTAS:


    // Callback from c4doc_put() that applies a delta, during _insertRevisionsNow()
    C4SliceResult Puller::applyDeltaCallback(const C4Revision *baseRevision,
                                             C4Slice deltaJSON,
                                             C4Error *outError)
    {
        Doc doc = _db->applyDelta(baseRevision, deltaJSON, true, outError);
        if (!doc)
            return {};
        alloc_slice body = doc.allocedData();

        if (!_db->disableBlobSupport()) {
            // After applying the delta, remove legacy attachment properties and any other
            // "_"-prefixed top level properties:
            Dict root = doc.root().asDict();
            if (c4doc_hasOldMetaProperties(root)) {
                _db->use([&](C4Database *db) {
                    C4Error err;
                    FLSharedKeys sk = c4db_getFLSharedKeys(db);
                    body = c4doc_encodeStrippingOldMetaProperties(root, sk, &err);
                    if (!body) {
                        warn("Failed to strip legacy attachments: error %d/%d", err.domain, err.code);
                        if (outError)
                            *outError = c4error_make(WebSocketDomain, 500, "invalid legacy attachments"_sl);
                    }
                });
            }
        }
        return C4SliceResult(body);
    }


#pragma mark - INSERTING & SYNCING REVISIONS:


    void Puller::insertRevision(RevToInsert *rev) {
        _revsToInsert.push(rev);
    }


    // Insert all the revisions queued for insertion, and sync the ones queued for syncing.
    void Puller::_insertRevisionsNow() {
        auto revs = _revsToInsert.pop();
        if (!revs)
            return;

        logVerbose("Inserting %zu revs:", revs->size());
        Stopwatch st;

        C4Error transactionErr = _db->inTransaction([&](C4Database *db, C4Error*) {
            // Before updating docs, write all pending changes to remote ancestors, in case any
            // of them apply to the docs we're updating:
            _db->markRevsSyncedNow();

            for (RevToInsert *rev : *revs) {
                // Add a revision:
                C4Error docErr;
                bool docSaved;

                if (rev->flags & kRevPurged) {
                    // Server says the document is no longer accessible, i.e. it's been
                    // removed from all channels the client has access to. Purge it.
                    docSaved = c4db_purgeDoc(db, rev->docID, &docErr);
                    if (docSaved)
                        logVerbose("    {'%.*s' removed (purged)}", SPLAT(rev->docID));
                    else if (docErr.domain == LiteCoreDomain && docErr.code == kC4ErrorNotFound)
                        docSaved = true;
                } else {
                    // Set up the parameter block for c4doc_put():
                    vector<C4String> history = rev->history();
                    C4DocPutRequest put = {};
                    put.docID = rev->docID;
                    put.revFlags = rev->flags;
                    put.existingRevision = true;
                    put.allowConflict = !rev->noConflicts;
                    put.history = history.data();
                    put.historyCount = history.size();
                    put.remoteDBID = _db->remoteDBID();
                    put.save = true;

                    alloc_slice bodyForDB;
                    if (rev->deltaSrc) {
                        // If this is a delta, put the JSON delta in the put-request:
                        bodyForDB = move(rev->deltaSrc);
                        put.deltaSourceRevID = rev->deltaSrcRevID;
                        put.deltaCB = [](void *context, const C4Revision *baseRev,
                                         C4Slice delta, C4Error *outError) {
                            return ((Puller*)context)->applyDeltaCallback(baseRev, delta, outError);
                        };
                        put.deltaCBContext = this;
                        // Preserve rev body as the source of a future delta I may push back:
                        put.revFlags |= kRevKeepBody;
                    } else {
                        // Encode doc body using database's real sharedKeys:
                        if (rev->doc)
                            bodyForDB = _db->reEncodeForDatabase(rev->doc);
                        rev->doc = nullptr;
                        // Preserve rev body as the source of a future delta I may push back:
                        if (bodyForDB.size >= tuning::kMinBodySizeForDelta && !_disableDeltaSupport)
                            put.revFlags |= kRevKeepBody;
                    }
                    put.allocedBody = {(void*)bodyForDB.buf, bodyForDB.size};

                    // The save!!
                    c4::ref<C4Document> doc = c4doc_put(db, &put, nullptr, &docErr);
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
                    // Notify owner of a rev that failed:
                    alloc_slice desc = c4error_getDescription(docErr);
                    warn("Failed to insert '%.*s' #%.*s : %.*s",
                         SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(desc));
                    rev->error = docErr;
                    if (rev->owner)
                        rev->owner->revisionInserted();
                }
            }
            return true;
        });

        if (transactionErr.code != 0)
            warn("Transaction failed!");

        // Notify owners of all revs that didn't already fail:
        for (auto rev : *revs) {
            if (rev->error.code == 0) {
                rev->error = transactionErr;
                if (rev->owner)
                    rev->owner->revisionInserted();
            }
        }

        if (transactionErr.code != 0) {
            gotError(transactionErr);
        } else {
            double t = st.elapsed();
            logInfo("Inserted %zu revs in %.2fms (%.0f/sec)", revs->size(), t*1000, revs->size()/t);
        }
    }



} }
