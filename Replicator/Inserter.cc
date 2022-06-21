//
// Inserter.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Inserter.hh"
#include "Replicator.hh"
#include "ReplicatorTuning.hh"
#include "IncomingRev.hh"
#include "DBAccess.hh"
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "Instrumentation.hh"
#include "c4ExceptionUtils.hh"
#include "c4Private.h"
#include "c4Document.hh"
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
    { }


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

        C4Error transactionErr = {};
        try {
            DBAccess::Transaction transaction(_db->insertionDB());
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
                    string desc = docErr.description();
                    warn("Failed to insert '%.*s' #%.*s : %s",
                         SPLAT(rev->docID), SPLAT(rev->revID), desc.c_str());
                    rev->error = docErr;
                    if (docErr == C4Error{LiteCoreDomain, kC4ErrorDeltaBaseUnknown}
                            || docErr == C4Error{LiteCoreDomain, kC4ErrorCorruptDelta})
                        rev->errorIsTransient = true;
                    rev->owner->revisionInserted();     // Tell the IncomingRev
                }
            }

            Stopwatch stCommit;
            transaction.commit();
            commitTime = st.elapsed();
        } catch (...) {
            transactionErr = C4Error::fromCurrentException();
            warn("Transaction failed!");
        }

        // Notify owners of all revs that didn't already fail:
        for (auto &rev : *revs) {
            if (rev->error.code == 0) {
                rev->error = transactionErr;
                rev->owner->revisionInserted();
            }
        }

        if (transactionErr) {
            gotError(transactionErr);
        } else {
            double t = st.elapsed();
            logInfo("Inserted %3zu revs in %6.2fms (%5.0f/sec) of which %4.1f%% was commit",
                    revs->size(), t*1000, revs->size()/t, commitTime/t*100);
        }
    }


    // Inserts one revision. Returns only C4Errors, never throws exceptions.
    bool Inserter::insertRevisionNow(RevToInsert *rev, C4Error *outError) {
        try {
            if (rev->flags & kRevPurged) {
                // Server says the document is no longer accessible, i.e. it's been
                // removed from all channels the client has access to. Purge it.
                if (_db->insertionDB().useLocked()->purgeDocument(rev->docID))
                    logVerbose("    {'%.*s' removed (purged)}", SPLAT(rev->docID));
                return true;

            } else {
                // Set up the "put" parameter block:
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
                                     C4Slice delta, C4Error *outError) -> C4SliceResult {
                        try {
                            return ((Inserter*)context)->applyDeltaCallback(doc, delta, outError);
                        } catch (...) {
                            *outError = C4Error::fromCurrentException();
                            return {};
                        }
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
                        && !_options->disableDeltaSupport())
                        put.revFlags |= kRevKeepBody;
                }
                put.allocedBody = {(void*)bodyForDB.buf, bodyForDB.size};

                // The save!!
                auto doc = _db->insertionDB().useLocked()->putDocument(put, nullptr, outError);
                if (!doc)
                    return false;
                logVerbose("    {'%.*s' #%.*s <- %.*s} seq %" PRIu64,
                           SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(rev->historyBuf),
                           (uint64_t)doc->selectedRev().sequence);
                rev->sequence = doc->selectedRev().sequence;
                if (doc->selectedRev().flags & kRevIsConflict) {
                    // Note that rev was inserted but caused a conflict:
                    logInfo("Created conflict with '%.*s' #%.*s",
                            SPLAT(rev->docID), SPLAT(rev->revID));
                    rev->flags |= kRevIsConflict;
                    rev->isWarning = true;
                    DebugAssert(put.allowConflict);
                }
                return true;
            }
        } catch (...) {
            *outError = C4Error::fromCurrentException();
            return false;
        }
    }


    // Callback from c4doc_put() that applies a delta, during _insertRevisionsNow()
    C4SliceResult Inserter::applyDeltaCallback(C4Document *c4doc,
                                               C4Slice deltaJSON,
                                               C4Error *outError)
    {
        Doc doc = _db->applyDelta(c4doc, deltaJSON, true);
        alloc_slice body = doc.allocedData();

        if (!_db->disableBlobSupport()) {
            // After applying the delta, remove legacy attachment properties and any other
            // "_"-prefixed top level properties:
            Dict root = doc.root().asDict();
            if (C4Document::hasOldMetaProperties(root)) {
                body = nullslice;
                try {
                    FLSharedKeys sk = _db->insertionDB().useLocked()->getFleeceSharedKeys();
                    body = C4Document::encodeStrippingOldMetaProperties(root, sk);
                } catchAndWarn();
                if (!body)
                    *outError = C4Error::make(WebSocketDomain, 500, "invalid legacy attachments");
            }
        }
        return C4SliceResult(body);
    }

} }
