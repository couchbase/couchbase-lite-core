//
// Pusher+DB.cc
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

#include "Pusher.hh"
#include "ReplicatorTuning.hh"
#include "Pusher.hh"
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Document+Fleece.h"
#include "c4Replicator.h"
#include "BLIP.hh"
#include "SecureRandomize.hh"

using namespace std;
using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

#pragma mark - CHANGES:


    // Gets the next batch of changes from the DB. Will respond by calling gotChanges.
    void Pusher::getChanges(const GetChangesParams &p)
    {
        if (!connection())
            return;
        auto limit = p.limit;
        logVerbose("Reading up to %u local changes since #%" PRIu64, limit, p.since);
        _getForeignAncestors = p.getForeignAncestors;
        _skipForeignChanges = p.skipForeign;
        _pushDocIDs = p.docIDs;
        if (_maxPushedSequence == 0)
            _maxPushedSequence = p.since;

        if (_getForeignAncestors)
            _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date

        // Run a by-sequence enumerator to find the changed docs:
        auto changes = make_shared<RevToSendList>();
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (!p.getForeignAncestors && !_options.pushFilter)
            options.flags &= ~kC4IncludeBodies;
        if (!p.skipDeleted)
            options.flags |= kC4IncludeDeleted;

        _db->use([&](C4Database* db) {
            c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(db, p.since, &options, &error);
            if (e) {
                changes->reserve(limit);
                while (c4enum_next(e, &error) && limit > 0) {
                    C4DocumentInfo info;
                    c4enum_getDocumentInfo(e, &info);
                    _maxPushedSequence = info.sequence;
                    auto rev = retained(new RevToSend(info));
                    if (shouldPushRev(rev, e, db)) {
                        changes->push_back(rev);
                        --limit;
                    }
                }
            }

            if (p.continuous && limit > 0 && !_changeObserver) {
                // Reached the end of history; now start observing for future changes
                _changeObserver = c4dbobs_create(db,
                                                 [](C4DatabaseObserver* observer, void *context) {
                                                     auto self = (Pusher*)context;
                                                     self->enqueue(&Pusher::dbChanged);
                                                 },
                                                 this);
                logDebug("Started DB observer");
            }
        });

        gotChanges(move(changes), _maxPushedSequence, error);
    }


    // (Async) callback from the C4DatabaseObserver when the database has changed
    void Pusher::dbChanged() {
        if (!_changeObserver)
            return; // if replication has stopped already by the time this async call occurs

        if (_getForeignAncestors)
            _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date

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
            logVerbose("Notified of %u db changes #%" PRIu64 " ... #%" PRIu64,
                       nChanges, c4changes[0].sequence, c4changes[nChanges-1].sequence);

            // Copy the changes into a vector of RevToSend:
            C4DatabaseChange *c4change = c4changes;
            _db->use([&](C4Database *db) {
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
                    if (shouldPushRev(rev, nullptr, db)) {
                        changes->push_back(rev);
                        if (changes->size() >= kMaxChanges) {
                            gotChanges(move(changes), _maxPushedSequence, {});
                            changes.reset();
                        }
                    }
                }
            });

            c4dbobs_releaseChanges(c4changes, nChanges);
        }

        if (changes && changes->size() > 0)
            gotChanges(move(changes), _maxPushedSequence, {});
    }


    // Common subroutine of _getChanges and dbChanged that adds a document to a list of Revs.
    bool Pusher::shouldPushRev(RevToSend *rev, C4DocEnumerator *e, C4Database *db) {
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
            logVerbose("Holding off on change '%.*s' %.*s till earlier rev is done",
                       SPLAT(rev->docID), SPLAT(rev->revID));
            active->second = rev;
            return false;
        }

        if (rev->expiration > 0 && rev->expiration < c4_now()) {
            logVerbose("'%.*s' is expired; not pushing it", SPLAT(rev->docID));
            return false;
        }

        bool needRemoteRevID = _getForeignAncestors && !rev->remoteAncestorRevID &&_checkpointValid;
        if (needRemoteRevID || _options.pushFilter) {
            c4::ref<C4Document> doc;
            C4Error error;
            doc = e ? c4enum_getDocument(e, &error) : c4doc_get(db, rev->docID, true, &error);
            if (!doc) {
                finishedDocumentWithError(rev, error, false);
                return false;   // reject rev: error getting doc
            }
            if (slice(doc->revID) != slice(rev->revID))
                return false;   // ignore rev: there's a newer one already

            if (needRemoteRevID) {
                // For proposeChanges, find the nearest foreign ancestor of the current rev:
                Assert(_db->remoteDBID());
                alloc_slice foreignAncestor = _db->getDocRemoteAncestor(doc);
                logDebug("remoteRevID of '%.*s' is %.*s", SPLAT(doc->docID), SPLAT(foreignAncestor));
                if (_skipForeignChanges && foreignAncestor == slice(rev->revID))
                    return false;   // skip this rev: it's already on the peer
                if (foreignAncestor
                        && c4rev_getGeneration(foreignAncestor) >= c4rev_getGeneration(rev->revID)) {
                    if (_options.pull <= kC4Passive) {
                        error = c4error_make(WebSocketDomain, 409,
                                             "conflicts with newer server revision"_sl);
                        finishedDocumentWithError(rev, error, false);
                    }
                    return false;    // ignore rev: there's a newer one on the server
                }
                rev->remoteAncestorRevID = foreignAncestor;
            }

            if (_options.pushFilter) {
                if (!_options.pushFilter(doc->docID, doc->selectedRev.flags,
                                         DBAccess::getDocRoot(doc), _options.callbackContext)) {
                    logVerbose("Doc '%.*s' rejected by push filter", SPLAT(doc->docID));
                    return false;
                }
            }
        }

        _pushingDocs.insert({rev->docID, nullptr});
        return true;
    }


#pragma mark - SENDING REVISIONS:


    // Sends a document revision in a "rev" request.
    void Pusher::sendRevision(RevToSend *request, MessageProgressCallback onProgress) {
        if (!connection())
            return;
        logVerbose("Reading document '%.*s' #%.*s",
                   SPLAT(request->docID), SPLAT(request->revID));

        // Get the document & revision:
        C4Error c4err;
        slice revisionBody;
        Dict root;
        c4::ref<C4Document> doc = _db->getDoc(request->docID, &c4err);
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
            msg.noreply = !onProgress;
            if (request->noConflicts)
                msg["noconflicts"_sl] = true;
            auto revisionFlags = doc->selectedRev.flags;
            if (revisionFlags & kRevDeleted)
                msg["deleted"_sl] = "1"_sl;
            string history = request->historyString(doc);
            if (!history.empty())
                msg["history"_sl] = history;

            bool sendLegacyAttachments = (request->legacyAttachments
                                          && (revisionFlags & kRevHasAttachments)
                                          && !_db->disableBlobSupport());

            // Delta compression:
            alloc_slice delta = createRevisionDelta(doc, request, root, revisionBody.size,
                                                    sendLegacyAttachments);
            if (delta) {
                msg["deltaSrc"_sl] = doc->selectedRev.revID;
                msg.jsonBody().writeRaw(delta);
            } else if (root.empty()) {
                msg.write("{}"_sl);
            } else {
                auto &bodyEncoder = msg.jsonBody();
                if (sendLegacyAttachments)
                    _db->encodeRevWithLegacyAttachments(bodyEncoder, root,
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
                couldntSendRevision(request);
        }
    }


    slice Pusher::getRevToSend(C4Document* doc, const RevToSend &request, C4Error *c4err) {
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


    alloc_slice Pusher::createRevisionDelta(C4Document *doc, RevToSend *request,
                                              Dict root, size_t revisionSize,
                                              bool sendLegacyAttachments)
    {
        alloc_slice delta;
        if (!request->deltaOK || revisionSize < tuning::kMinBodySizeForDelta
                              || _options.disableDeltaSupport())
            return delta;

        // Find an ancestor revision known to the server:
        C4RevisionFlags ancestorFlags = 0;
        Dict ancestor;
        if (request->remoteAncestorRevID)
            ancestor = DBAccess::getDocRoot(doc, request->remoteAncestorRevID, &ancestorFlags);
        if (!ancestor && request->ancestorRevIDs) {
            for (auto revID : *request->ancestorRevIDs) {
                ancestor = DBAccess::getDocRoot(doc, revID, &ancestorFlags);
                if (ancestor)
                    break;
            }
        }
        if (!ancestor)
            return delta;

        Doc legacyOld, legacyNew;
        if (sendLegacyAttachments) {
            // If server needs legacy attachment layout, transform the bodies:
            Encoder enc;
            auto revPos = c4rev_getGeneration(request->revID);
            _db->encodeRevWithLegacyAttachments(enc, root, revPos);
            legacyNew = enc.finishDoc();
            root = legacyNew.root().asDict();

            if (ancestorFlags & kRevHasAttachments) {
                enc.reset();
                _db->encodeRevWithLegacyAttachments(enc, ancestor, revPos);
                legacyOld = enc.finishDoc();
                ancestor = legacyOld.root().asDict();
            }
        }

        delta = FLCreateJSONDelta(ancestor, root);
        if (!delta || delta.size > revisionSize * 1.2)
            return {};          // Delta failed, or is (probably) bigger than body; don't use

        if (willLog(LogLevel::Verbose)) {
            alloc_slice old (ancestor.toJSON());
            alloc_slice nuu (root.toJSON());
            logVerbose("Encoded revision as delta, saving %zd bytes:\n\told = %.*s\n\tnew = %.*s\n\tDelta = %.*s",
                       nuu.size - delta.size,
                       SPLAT(old), SPLAT(nuu), SPLAT(delta));
        }
        return delta;
    }

} }
