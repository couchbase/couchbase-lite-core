//
// DBWorker+Push.cc
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


    void DBWorker::getChanges(const GetChangesParams &params, Pusher *pusher) {
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
                if (shouldPushRev(rev, e)) {
                    changes->push_back(rev);
                    --p.limit;
                }
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
                if (shouldPushRev(rev, nullptr)) {
                    changes->push_back(rev);
                    if (changes->size() >= kMaxChanges) {
                        _pusher->gotChanges(move(changes), _maxPushedSequence, {});
                        changes.reset();
                    }
                }
            }

            c4dbobs_releaseChanges(c4changes, nChanges);
        }

        if (changes && changes->size() > 0)
            _pusher->gotChanges(move(changes), _maxPushedSequence, {});
    }


    // Common subroutine of _getChanges and dbChanged that adds a document to a list of Revs.
    bool DBWorker::shouldPushRev(RevToSend *rev,
                                   C4DocEnumerator *e)
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
                                         getDocRoot(doc), _options.callbackContext)) {
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
            msg.noreply = !onProgress;
            if (request->noConflicts)
                msg["noconflicts"_sl] = true;
            auto revisionFlags = doc->selectedRev.flags;
            if (revisionFlags & kRevDeleted)
                msg["deleted"_sl] = "1"_sl;
            string history = revHistoryString(doc, *request);
            if (!history.empty())
                msg["history"_sl] = history;

            bool sendLegacyAttachments = (request->legacyAttachments
                                          && (revisionFlags & kRevHasAttachments)
                                          && !_disableBlobSupport);

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
                sprintf(fakeID, "%u-faded000%.08x%.08x", lastGen, RandomNumber(), RandomNumber());
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


    alloc_slice DBWorker::createRevisionDelta(C4Document *doc, RevToSend *request,
                                              Dict root, size_t revisionSize,
                                              bool sendLegacyAttachments)
    {
        alloc_slice delta;
        if (!request->deltaOK || _disableDeltaSupport
                              || revisionSize < tuning::kMinBodySizeForDelta)
            return delta;

        // Find an ancestor revision known to the server:
        C4RevisionFlags ancestorFlags = 0;
        Dict ancestor;
        if (request->remoteAncestorRevID)
            ancestor = getDocRoot(doc, request->remoteAncestorRevID, &ancestorFlags);

        if(ancestorFlags & kRevDeleted) {
            return delta;
        }

        if (!ancestor && request->ancestorRevIDs) {
            for (auto revID : *request->ancestorRevIDs) {
                ancestor = getDocRoot(doc, revID, &ancestorFlags);
                if (ancestor)
                    break;
            }
        }
        if (ancestor.empty())
            return delta;

        Doc legacyOld, legacyNew;
        if (sendLegacyAttachments) {
            // If server needs legacy attachment layout, transform the bodies:
            Encoder enc;
            auto revPos = c4rev_getGeneration(request->revID);
            writeRevWithLegacyAttachments(enc, root, revPos);
            legacyNew = enc.finishDoc();
            root = legacyNew.root().asDict();

            if (ancestorFlags & kRevHasAttachments) {
                enc.reset();
                writeRevWithLegacyAttachments(enc, ancestor, revPos);
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
        findBlobReferences(root, false, [&](FLDeepIterator di, FLDict blob, C4BlobKey blobKey) {
            alloc_slice path(FLDeepIterator_GetJSONPointer(di));
            if (path.hasPrefix("/_attachments/"_sl))
                return;
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


    void DBWorker::_donePushingRev(RetainedConst<RevToSend> rev, bool synced) {
        if (synced && _options.push > kC4Passive)
            _revsToMarkSynced.push((RevToSend*)rev.get());

        auto i = _pushingDocs.find(rev->docID);
        if (i == _pushingDocs.end()) {
            if (connection())
                warn("_donePushingRev('%.*s'): That docID is not active!", SPLAT(rev->docID));
            return;
        }

        Retained<RevToSend> newRev = i->second;
        _pushingDocs.erase(i);
        if (newRev) {
            if (synced && _getForeignAncestors)
                newRev->remoteAncestorRevID = rev->revID;
            logVerbose("Now that '%.*s' %.*s is done, propose %.*s (remote %.*s) ...",
                       SPLAT(rev->docID), SPLAT(rev->revID), SPLAT(newRev->revID),
                       SPLAT(newRev->remoteAncestorRevID));
            bool ok = false;
            if (synced && _getForeignAncestors
                       && c4rev_getGeneration(newRev->revID) <= c4rev_getGeneration(rev->revID)) {
                // Don't send; it'll conflict with what's on the server
            } else {
                // Send newRev as though it had just arrived:
                auto changes = make_shared<RevToSendList>();
                if (shouldPushRev(newRev, nullptr)) {
                    _maxPushedSequence = max(_maxPushedSequence, rev->sequence);
                    _pusher->gotOutOfOrderChange(newRev);
                    ok = true;
                }
            }
            if (!ok) {
                logVerbose("   ... nope, decided not to propose '%.*s' %.*s",
                           SPLAT(newRev->docID), SPLAT(newRev->revID));
            }
        } else {
            logDebug("Done pushing '%.*s' %.*s", SPLAT(rev->docID), SPLAT(rev->revID));
        }
    }

} }
