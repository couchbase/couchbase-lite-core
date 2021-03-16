//
// Inserter.cc
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

#include "Inserter.hh"
#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "IncomingRev.hh"
#include "DBAccess.hh"
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "Instrumentation.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4ReplicatorTypes.h"
#include "BLIP.hh"

using namespace std;
using namespace fleece;
using namespace litecore::blip;


namespace litecore { namespace repl {


    Inserter::Inserter(Replicator *repl)
    :Worker(repl, "Insert")
    ,_revsToInsert(this, "revsToInsert", &Inserter::_insertRevisionsNow,
                   tuning::kInsertionDelay, tuning::kInsertionBatchSize)
    {
        _passive = _options.pull <= kC4Passive;
    }


    void Inserter::insertRevision(RevToInsert *rev) {
        _revsToInsert.push(rev);
    }


    // Inserts all the revisions queued for insertion.
    void Inserter::_insertRevisionsNow(int gen) {
        auto revs = _revsToInsert.pop(gen);
        if (!revs)
            return;

        logVerbose("Inserting %zu revs:", revs->size());
        Stopwatch st;
        double commitTime = 0;

        DBAccess::Transaction transaction(*_db);
        C4Error transactionErr;
        if (transaction.begin(&transactionErr)) {
            // Before updating docs, write all pending changes to remote ancestors, in case any
            // of them apply to the docs we're updating:
            _db->markRevsSyncedNow();

            for (RevToInsert *rev : *revs) {
                C4Error docErr;
                bool docSaved = insertRevisionNow(rev, &docErr);
                rev->trimBody();                // don't need body any more
                if (docSaved) {
                    rev->owner->revisionProvisionallyInserted();
                } else {
                    // Notify owner of a rev that failed:
                    alloc_slice desc = c4error_getDescription(docErr);
                    warn("Failed to insert '%.*s' #%.*s : %.*s",
                         SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(desc));
                    rev->error = docErr;
                    if (docErr == C4Error{LiteCoreDomain, kC4ErrorDeltaBaseUnknown}
                            || docErr == C4Error{LiteCoreDomain, kC4ErrorCorruptDelta})
                        rev->errorIsTransient = true;
                    rev->owner->revisionInserted();     // Tell the IncomingRev
                }
            }

            Stopwatch stCommit;
            if (transaction.commit(&transactionErr))
                transactionErr = {};
            commitTime = st.elapsed();
        }

        if (transactionErr.code != 0)
            warn("Transaction failed!");

        // Notify owners of all revs that didn't already fail:
        for (auto &rev : *revs) {
            if (rev->error.code == 0) {
                rev->error = transactionErr;
                rev->owner->revisionInserted();
            }
        }

        if (transactionErr.code != 0) {
            gotError(transactionErr);
        } else {
            double t = st.elapsed();
            logInfo("Inserted %3zu revs in %6.2fms (%5.0f/sec) of which %4.1f%% was commit",
                    revs->size(), t*1000, revs->size()/t, commitTime/t*100);
        }
    }


    // Inserts one revision.
    bool Inserter::insertRevisionNow(RevToInsert *rev, C4Error *outError) {
        if (rev->flags & kRevPurged) {
            // Server says the document is no longer accessible, i.e. it's been
            // removed from all channels the client has access to. Purge it.
            bool purged;
            _db->useForInsert([&](C4Database *idb) {
                purged = c4db_purgeDoc(idb, rev->docID, outError);
            });
            if (purged)
                logVerbose("    {'%.*s' removed (purged)}", SPLAT(rev->docID));
            else if (outError->domain == LiteCoreDomain && outError->code == kC4ErrorNotFound)
                purged = true;
            return purged;

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
                put.deltaCB = [](void *context, C4Document *doc,
                                 C4Slice delta, C4Error *outError) {
                    return ((Inserter*)context)->applyDeltaCallback(doc, delta, outError);
                };
                put.deltaCBContext = this;
                // Preserve rev body as the source of a future delta I may push back:
                put.revFlags |= kRevKeepBody;
            } else {
                // If not a delta, encode doc body using database's real sharedKeys:
                bodyForDB = _db->reEncodeForDatabase(rev->doc);
                rev->doc = nullptr;
                // Preserve rev body as the source of a future delta I may push back:
                if (bodyForDB.size >= tuning::kMinBodySizeForDelta
                    && !_options.disableDeltaSupport())
                    put.revFlags |= kRevKeepBody;
            }
            put.allocedBody = {(void*)bodyForDB.buf, bodyForDB.size};

            // The save!!
            return _db->useForInsert<bool>([&](C4Database *db) {
                c4::ref<C4Document> doc = c4doc_put(db, &put, nullptr, outError);
                if (!doc)
                    return false;
                logVerbose("    {'%.*s' #%.*s <- %.*s} seq %" PRIu64,
                           SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(rev->historyBuf),
                           doc->selectedRev.sequence);
                rev->sequence = doc->selectedRev.sequence;
                if (doc->selectedRev.flags & kRevIsConflict) {
                    // Note that rev was inserted but caused a conflict:
                    logInfo("Created conflict with '%.*s' #%.*s",
                            SPLAT(rev->docID), SPLAT(rev->revID));
                    rev->flags |= kRevIsConflict;
                    rev->isWarning = true;
                    DebugAssert(put.allowConflict);
                }
                return true;
            });
        }
    }


    // Callback from c4doc_put() that applies a delta, during _insertRevisionsNow()
    C4SliceResult Inserter::applyDeltaCallback(C4Document *c4doc,
                                               C4Slice deltaJSON,
                                               C4Error *outError)
    {
        Doc doc = _db->applyDelta(c4doc, deltaJSON, true, outError);
        if (!doc)
            return {};
        alloc_slice body = doc.allocedData();

        if (!_db->disableBlobSupport()) {
            // After applying the delta, remove legacy attachment properties and any other
            // "_"-prefixed top level properties:
            Dict root = doc.root().asDict();
            if (c4doc_hasOldMetaProperties(root)) {
                _db->useForInsert([&](C4Database *idb) {
                    C4Error err;
                    FLSharedKeys sk = c4db_getFLSharedKeys(idb);
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

} }
