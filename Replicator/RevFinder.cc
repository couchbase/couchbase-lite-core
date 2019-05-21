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
    { }


    // Called by the Puller; handles a "changes" or "proposeChanges" message by checking which of
    // the changes don't exist locally, and returning a bit-vector indicating them.
    void RevFinder::_findOrRequestRevs(Retained<MessageIn> req,
                                       std::function<void(std::vector<bool>)> completion)
    {
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
        if ( !_announcedDeltaSupport && !_options.disableDeltaSupport()) {
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

        completion(move(whichRequested));

        req->respond(response);
        logInfo("Responded to '%.*s' REQ#%" PRIu64 " w/request for %u revs",
            SPLAT(req->property("Profile"_sl)), req->number(), requested);
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


    // Returns true if revision exists; else returns false and sets ancestors to an array of
    // ancestor revisions I do have (empty if doc doesn't exist at all)
    bool RevFinder::findAncestors(slice docID, slice revID, vector<alloc_slice> &ancestors) {
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

        bool disableDeltaSupport = _options.disableDeltaSupport();
        auto addAncestor = [&]() {
            if (disableDeltaSupport || c4doc_hasRevisionBody(doc))  // need body for deltas
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
    void RevFinder::updateRemoteRev(C4Document *doc) {
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

} }
