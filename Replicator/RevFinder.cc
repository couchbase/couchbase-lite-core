//
// RevFinder.cc
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

#include "RevFinder.hh"
#include "ReplicatorTuning.hh"
#include "IncomingRev.hh"
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "Instrumentation.hh"
#include "c4.hh"
#include "c4Transaction.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "BLIP.hh"

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    RevFinder::RevFinder(Replicator *replicator)
    :Worker(replicator, "RevFinder")
    {
        _passive = _options.pull <= kC4Passive;
    }


    void RevFinder::onError(C4Error err) {
        // If the database closes on replication stop, this error might happen
        // but it is inconsequential so suppress it.  It will still be logged, but
        // not in the worker's error property.
        if(err.domain != LiteCoreDomain || err.code != kC4ErrorNotOpen) {
            Worker::onError(err);
        }
    }


    // Called by the Puller; handles a "changes" or "proposeChanges" message by checking which of
    // the changes don't exist locally, and returning a bit-vector indicating them.
    void RevFinder::_findOrRequestRevs(Retained<MessageIn> req,
                                       DocIDMultiset *incomingDocs,
                                       std::function<void(std::vector<bool>)> completion)
    {
        bool proposed = (req->property("Profile"_sl) == "proposeChanges"_sl);
        auto changes = req->JSONBody().asArray();
        auto nChanges = changes.count();
        if (willLog() && !changes.empty()) {
            if (proposed) {
                logInfo("Received %u changes", nChanges);
            } else {
                alloc_slice firstSeq(changes[0].asArray()[0].toString());
                alloc_slice lastSeq (changes[nChanges-1].asArray()[0].toString());
                logInfo("Received %u changes (seq '%.*s'..'%.*s')",
                    nChanges, SPLAT(firstSeq), SPLAT(lastSeq));
            }
        }

        if (!proposed)
            _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date

        MessageBuilder response(req);
        response.compressed = true;
        _db->use([&](C4Database *db) {
            DBAccess::AssertDBOpen(db);

            response["maxHistory"_sl] = c4db_getMaxRevTreeDepth(db);
        });
        if (!_db->disableBlobSupport())
            response["blobs"_sl] = "true"_sl;
        if ( !_announcedDeltaSupport && !_options.disableDeltaSupport()) {
            response["deltas"_sl] = "true"_sl;
            _announcedDeltaSupport = true;
        }

        Stopwatch st;

        vector<bool> whichRequested(nChanges);
        unsigned itemsWritten = 0, requested = 0;

        auto &encoder = response.jsonBody();
        encoder.beginArray();

        if (proposed) {
            // Proposed changes (peer is LiteCore):
            vector<alloc_slice> ancestors;
            int i = -1;
            for (auto item : changes) {
                ++i;
                // Look up each revision in the `req` list:
                // "proposeChanges" entry: [docID, revID, parentRevID?, bodySize?]
                // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
                auto change = item.asArray();
                alloc_slice docID( change[proposed ? 0 : 1].asString() );
                slice revID = change[proposed ? 1 : 2].asString();
                if (docID.size == 0 || revID.size == 0) {
                    warn("Invalid entry in 'changes' message");
                    continue;     // ???  Should this abort the replication?
                }

                slice parentRevID = change[2].asString();
                if (parentRevID.size == 0)
                    parentRevID = nullslice;
                alloc_slice currentRevID;
                int status = findProposedChange(docID, revID, parentRevID, currentRevID);
                if (status == 0) {
                    // Accept rev by (lazily) appending a 0:
                    logDebug("    - Accepting proposed change '%.*s' #%.*s with parent %.*s",
                             SPLAT(docID), SPLAT(revID), SPLAT(parentRevID));
                    ++requested;
                    whichRequested[i] = true;
                } else {
                    // Reject rev by appending status code:
                    logInfo("Rejecting proposed change '%.*s' #%.*s with parent %.*s (status %d; current rev is %.*s)",
                        SPLAT(docID), SPLAT(revID), SPLAT(parentRevID), status, SPLAT(currentRevID));
                    while (itemsWritten++ < i)
                        encoder.writeInt(0);
                    encoder.writeInt(status);
                }
            }

        } else {
            // Non-proposed changes:
            // Compile the docIDs/revIDs into parallel vectors:
            vector<slice> docIDs, revIDs;
            docIDs.reserve(nChanges);
            revIDs.reserve(nChanges);
            for (auto item : changes) {
                // "changes" entry: [sequence, docID, revID, deleted?, bodySize?]
                auto change = item.asArray();
                docIDs.push_back(change[1].asString());
                revIDs.push_back(change[2].asString());
            }

            // Ask the database to look up the ancestors:
            vector<C4StringResult> ancestors(nChanges);
            C4Error err;
            bool ok = _db->use<bool>([&](C4Database *db) {
                return c4db_findDocAncestors(db, nChanges, kMaxPossibleAncestors,
                                             !_options.disableDeltaSupport(),  // requireBodies
                                             _db->remoteDBID(),
                                             (C4String*)docIDs.data(), (C4String*)revIDs.data(),
                                             ancestors.data(), &err);
            });
            if (!ok) {
                gotError(err);
            } else {
                // Look through the database response:
                for (size_t i = 0; i < nChanges; ++i) {
                    alloc_slice docID(docIDs[i]);
                    alloc_slice revID(revIDs[i]);
                    alloc_slice anc(std::move(ancestors[i]));
                    if (anc == kC4AncestorExistsButNotCurrent) {
                        // This means the rev exists but is not marked as the latest from the
                        // remote server, so I better make it so:
                        logDebug("    - Already have '%.*s' %.*s but need to mark it as remote ancestor",
                                 SPLAT(docID), SPLAT(revID));
                        _db->setDocRemoteAncestor(docID, revID);
                        replicator()->docRemoteAncestorChanged(docID, revID);
                    } else if (anc != kC4AncestorExists) {
                        // Don't have revision -- request it:
                        ++requested;
                        whichRequested[i] = true;
                        incomingDocs->add(docID);
                        
                        if (itemsWritten > 0)
                            // Sanity Check: This means we've hit this loop at least once so the current
                            // data is both non empty, and does not end in a comma
                            encoder.writeRaw(","_sl);
                        
                        // Append zeros for any items I skipped:
                        while (itemsWritten++ < i)
                            // CBL-1208: Comma logic is not always reliable, so write the comma first (see above)
                            // and then write a literal 0 character (skipping the comma logic in the encoder)
                            // and a comma because we are assured that we then write another entry a few lines down
                            encoder.writeRaw("0,");
                        
                        // Append array of ancestor revs I do have (it's already a JSON array):
                        // Note this does not end in a comma, so the next time around we will write the
                        // comma in the above writeRaw(",")
                        encoder.writeRaw(anc ? slice(anc) : "[]"_sl);
                        logDebug("    - Requesting '%.*s' %.*s, ancestors %.*s",
                                 SPLAT(docID), SPLAT(revID), SPLAT(anc));
                    }
                }
            }
        }

        completion(move(whichRequested));

        encoder.endArray();
        req->respond(response);
        logInfo("Responded to '%.*s' REQ#%" PRIu64 " w/request for %u revs in %.6f sec",
            SPLAT(req->property("Profile"_sl)), req->number(), requested, st.elapsed());
    }


    // Checks whether the revID (if any) is really current for the given doc.
    // Returns an HTTP-ish status code: 0=OK, 409=conflict, 500=internal error
    int RevFinder::findProposedChange(slice docID, slice revID, slice parentRevID,
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


} }
