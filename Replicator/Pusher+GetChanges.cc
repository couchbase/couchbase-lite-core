//
// Pusher+GetChanges.cc
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
#include "fleece/Fleece.hh"
#include "StringUtil.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4DocEnumerator.h"
#include "c4Document+Fleece.h"
#include "c4DocEnumerator.h"
#include "c4Observer.h"

using namespace std;
using namespace fleece;

namespace litecore { namespace repl {

    // Gets the next batch of changes from the DB. Will respond by calling gotChanges.
    void Pusher::getMoreChanges() {
        if (!connected())
            return;

        if (_changeObserver) {
            // We've caught up, so read from the observer:
            getObservedChanges();
            return;
        }

        logVerbose("Reading up to %u local changes since #%" PRIu64,
                   _changesBatchSize, _lastSequenceRead);
        if (_maxPushedSequence == 0)
            _maxPushedSequence = _lastSequenceRead;

        if (getForeignAncestors())
            _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date

        // Run a by-sequence enumerator to find the changed docs:
        auto changes = make_shared<RevToSendList>();
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (!getForeignAncestors() && !_options.pushFilter)
            options.flags &= ~kC4IncludeBodies;
        if (!_skipDeleted)
            options.flags |= kC4IncludeDeleted;

        _db->use([&](C4Database* db) {
            auto limit = _changesBatchSize;
            c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(db, _lastSequenceRead, &options, &error);
            if (e) {
                changes->reserve(limit);
                while (c4enum_next(e, &error) && limit > 0) {
                    C4DocumentInfo info;
                    c4enum_getDocumentInfo(e, &info);
                    auto rev = revToSend(info, e, db);
                    if (rev) {
                        changes->push_back(rev);
                        --limit;
                    }
                }
            }

            if (_continuous && limit > 0 && !_changeObserver) {
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


    void Pusher::getObservedChanges() {
        static const uint32_t kMaxChanges = 100;
        C4DatabaseChange c4changes[kMaxChanges];
        bool ext;
        uint32_t nChanges;
        shared_ptr<RevToSendList> changes;
        bool updateForeignAncestors = getForeignAncestors();

        while (0 != (nChanges = c4dbobs_getChanges(_changeObserver, c4changes, kMaxChanges, &ext))){
            if (!ext) {
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
                if (updateForeignAncestors) {
                    _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date
                    updateForeignAncestors = false;
                }

                for (uint32_t i = 0; i < nChanges; ++i, ++c4change) {
                    if (!changes) {
                        changes = make_shared<RevToSendList>();
                        changes->reserve(nChanges - i);
                    }
                    C4DocumentInfo info {0, c4change->docID, c4change->revID,
                                         c4change->sequence, c4change->bodySize};
                    // Note: we send tombstones even if the original getChanges() call specified
                    // skipDeletions. This is intentional; skipDeletions applies only to the initial
                    // dump of existing docs, not to 'live' changes.
                    auto rev = revToSend(info, nullptr, db);
                    if (rev) {
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

        if (changes) {
            gotChanges(move(changes), _maxPushedSequence, {});
        } else {
            logVerbose("Waiting for db changes...");
            _waitingForObservedChanges = true;
        }
    }


    // (Async) callback from the C4DatabaseObserver when the database has changed
    void Pusher::dbChanged() {
        if (!_changeObserver)
            return; // if replication has stopped already by the time this async call occurs
        logVerbose("Database changed!");
        if (_waitingForObservedChanges) {
            _waitingForObservedChanges = false;
            getObservedChanges();
        }
    }


    // Common subroutine of _getChanges and dbChanged that adds a document to a list of Revs.
    // It does some quick tests, and if those pass creates a RevToSend and passes it on to the
    // other shouldPushRev, which does more expensive checks.
    Retained<RevToSend> Pusher::revToSend(C4DocumentInfo &info, C4DocEnumerator *e, C4Database *db)
    {
        _maxPushedSequence = info.sequence;
        if (info.expiration > 0 && info.expiration < c4_now()) {
            logVerbose("'%.*s' is expired; not pushing it", SPLAT(info.docID));
            return nullptr;             // skip rev: expired
        } else if (!passive() && _checkpointer.isSequenceCompleted(info.sequence)) {
            return nullptr;             // skip rev: checkpoint says we already pushed it before
        } else if (_docIDs != nullptr
                    && _docIDs->find(slice(info.docID).asString()) == _docIDs->end()) {
            return nullptr;             // skip rev: not in list of docIDs
        } else {
            auto rev = retained(new RevToSend(info));
            return shouldPushRev(rev, e, db) ? rev : nullptr;
        }
    }


    // This is called both by the above version of shouldPushRev, and by doneWithRev.
    bool Pusher::shouldPushRev(Retained<RevToSend> rev, C4DocEnumerator *e, C4Database *db) {
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
            if (!passive())
                _checkpointer.addPendingSequence(rev->sequence);
            return false;             // defer rev: already sending a previous revision
        }

        bool needRemoteRevID = getForeignAncestors() && !rev->remoteAncestorRevID &&_checkpointValid;
        if (needRemoteRevID || _options.pushFilter) {
            c4::ref<C4Document> doc;
            C4Error error;
            doc = e ? c4enum_getDocument(e, &error) : c4doc_get(db, rev->docID, true, &error);
            if (!doc) {
                finishedDocumentWithError(rev, error, false);
                return false;         // fail the rev: error getting doc
            }
            if (slice(doc->revID) != slice(rev->revID))
                return false;         // skip rev: there's a newer one already

            if (needRemoteRevID) {
                // For proposeChanges, find the nearest foreign ancestor of the current rev:
                if (!getRemoteRevID(rev, doc))
                    return false;     // skip or fail rev: it's already on the peer
            }
            if (_options.pushFilter) {
                // If there's a push filter, ask it whether to push the doc:
                if (!_options.pushFilter(doc->docID, doc->selectedRev.revID, doc->selectedRev.flags,
                                         DBAccess::getDocRoot(doc), _options.callbackContext)) {
                    logVerbose("Doc '%.*s' rejected by push filter", SPLAT(doc->docID));
                    return false;     // skip rev: rejected by push filter
                }
            }
        }

        _pushingDocs.insert({rev->docID, nullptr});
        return true;
    }


    // Assigns rev->remoteAncestorRevID based on the document.
    // Returns false to reject the document if the remote is equal to or newer than this rev.
    bool Pusher::getRemoteRevID(RevToSend *rev, C4Document *doc) {
        // For proposeChanges, find the nearest foreign ancestor of the current rev:
        Assert(_db->remoteDBID());
        alloc_slice foreignAncestor = _db->getDocRemoteAncestor(doc);
        logDebug("remoteRevID of '%.*s' is %.*s", SPLAT(doc->docID), SPLAT(foreignAncestor));
        if (_proposeChanges && foreignAncestor == slice(doc->revID))
            return false;   // skip this rev: it's already on the peer
        if (foreignAncestor
                    && c4rev_getGeneration(foreignAncestor) >= c4rev_getGeneration(doc->revID)) {
            if (_options.pull <= kC4Passive) {
                C4Error error = c4error_make(WebSocketDomain, 409,
                                     "conflicts with newer server revision"_sl);
                finishedDocumentWithError(rev, error, false);
            }
            return false;    // skip or fail rev: there's a newer one on the peer
        }
        rev->remoteAncestorRevID = foreignAncestor;
        return true;
    }

} }
