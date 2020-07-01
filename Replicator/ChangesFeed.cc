//
// ChangesFeed.cc
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

#include "ChangesFeed.hh"
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

namespace litecore { namespace repl {


    ChangesFeed::ChangesFeed(Pusher& pusher, Options &options,
                             std::shared_ptr<DBAccess> db, Checkpointer &checkpointer)
    :Logging(SyncLog)
    ,_pusher(pusher)
    ,_options(options)
    ,_db(db)
    ,_checkpointer(checkpointer)
    ,_skipDeleted(_options.skipDeleted())
    ,_passive(_options.push <= kC4Passive)
    {
        filterByDocIDs(_options.docIDs());
    }


    void ChangesFeed::filterByDocIDs(Array docIDs) {
        if (!docIDs)
            return;
        DocIDSet combined(new unordered_set<string>);
        combined->reserve(docIDs.count());
        for (Array::iterator i(docIDs); i; ++i) {
            string docID = i.value().asstring();
            if (!docID.empty() && (!_docIDs || _docIDs->find(docID) != _docIDs->end()))
                combined->insert(move(docID));
        }
        _docIDs = move(combined);
        if (_passive)
            logInfo("Peer requested filtering to %zu docIDs", _docIDs->size());
    }


    // Gets the next batch of changes from the DB. Will respond by calling gotChanges.
    void ChangesFeed::getMoreChanges() {
        logVerbose("Asking DB for %u changes since sequence #%" PRIu64 " ...",
                   tuning::kDefaultChangeBatchSize, _maxPushedSequence);

        if (_changeObserver) {
            // We've caught up, so read from the observer:
            getObservedChanges();
            return;
        }

        logVerbose("Reading up to %u local changes since #%" PRIu64,
                   tuning::kDefaultChangeBatchSize, _maxPushedSequence);

        if (_getForeignAncestors)
            _db->markRevsSyncedNow();   // make sure foreign ancestors are up to date

        // Run a by-sequence enumerator to find the changed docs:
        auto changes = make_shared<RevToSendList>();
        C4Error error = {};
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (!_getForeignAncestors && !_options.pushFilter)
            options.flags &= ~kC4IncludeBodies;
        if (!_skipDeleted)
            options.flags |= kC4IncludeDeleted;

        _db->use([&](C4Database* db) {
            auto limit = tuning::kDefaultChangeBatchSize;
            c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(db, _maxPushedSequence, &options, &error);
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

            if (_options.push == kC4Continuous && limit > 0 && !_changeObserver) {
                // Reached the end of history; now start observing for future changes.
                // The callback runs on an arbitrary thread; for thread-safety, let the Pusher
                // handle it, since it's an Actor. The Pusher will end up calling my own
                // dbChanged() method.
                _changeObserver = c4dbobs_create(db,
                                                 [](C4DatabaseObserver* observer, void *context) {
                                                     auto pusher = (Pusher*)context;
                                                     pusher->dbChanged();
                                                 },
                                                 &_pusher);
                logDebug("Started DB observer");
            }
        });

        _pusher.gotChanges(move(changes), _maxPushedSequence, error);
    }


    void ChangesFeed::getObservedChanges() {
        static const uint32_t kMaxChanges = 100;
        C4DatabaseChange c4changes[kMaxChanges];
        bool ext;
        uint32_t nChanges;
        shared_ptr<RevToSendList> changes;
        bool updateForeignAncestors = _getForeignAncestors;

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
                            _pusher.gotChanges(move(changes), _maxPushedSequence, {});
                            changes.reset();
                        }
                    }
                }
            });

            c4dbobs_releaseChanges(c4changes, nChanges);
        }

        if (changes) {
            _pusher.gotChanges(move(changes), _maxPushedSequence, {});
        } else {
            logVerbose("Waiting for db changes...");
            _waitingForObservedChanges = true;
        }
    }


    // (Async) callback from the C4DatabaseObserver when the database has changed
    void ChangesFeed::dbChanged() {
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
    Retained<RevToSend> ChangesFeed::revToSend(C4DocumentInfo &info, C4DocEnumerator *e, C4Database *db)
    {
        _maxPushedSequence = info.sequence;
        if (info.expiration > 0 && info.expiration < c4_now()) {
            logVerbose("'%.*s' is expired; not pushing it", SPLAT(info.docID));
            return nullptr;             // skip rev: expired
        } else if (!_passive && _checkpointer.isSequenceCompleted(info.sequence)) {
            return nullptr;             // skip rev: checkpoint says we already pushed it before
        } else if (_docIDs != nullptr
                    && _docIDs->find(slice(info.docID).asString()) == _docIDs->end()) {
            return nullptr;             // skip rev: not in list of docIDs
        } else {
            auto rev = retained(new RevToSend(info));
            return shouldPushRev(rev, e, db) ? rev : nullptr;
        }
    }


    bool ChangesFeed::shouldPushRev(RevToSend *rev) {
        return _db->use<bool>([&](C4Database *db) {
            return shouldPushRev(rev, nullptr, db);
        });
    }


    // This is called both by revToSend, and by Pusher::doneWithRev.
    bool ChangesFeed::shouldPushRev(RevToSend *rev, C4DocEnumerator *e, C4Database *db) {
        bool needRemoteRevID = _getForeignAncestors && !rev->remoteAncestorRevID
                                                    && _pusher.isCheckpointValid();
        if (needRemoteRevID || _options.pushFilter) {
            c4::ref<C4Document> doc;
            C4Error error;
            doc = e ? c4enum_getDocument(e, &error) : c4doc_get(db, rev->docID, true, &error);
            if (!doc) {
                _pusher.failedToGetChange(rev, error, false);
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
        return true;
    }


    // Assigns rev->remoteAncestorRevID based on the document.
    // Returns false to reject the document if the remote is equal to or newer than this rev.
    bool ChangesFeed::getRemoteRevID(RevToSend *rev, C4Document *doc) {
        // For proposeChanges, find the nearest foreign ancestor of the current rev:
        Assert(_db->remoteDBID());
        alloc_slice foreignAncestor = _db->getDocRemoteAncestor(doc);
        logDebug("remoteRevID of '%.*s' is %.*s", SPLAT(doc->docID), SPLAT(foreignAncestor));
        if (_getForeignAncestors && foreignAncestor == slice(doc->revID))
            return false;   // skip this rev: it's already on the peer
        if (foreignAncestor
                    && c4rev_getGeneration(foreignAncestor) >= c4rev_getGeneration(doc->revID)) {
            if (_options.pull <= kC4Passive) {
                C4Error error = c4error_make(WebSocketDomain, 409,
                                     "conflicts with newer server revision"_sl);
                _pusher.failedToGetChange(rev, error, false);
            }
            return false;    // skip or fail rev: there's a newer one on the peer
        }
        rev->remoteAncestorRevID = foreignAncestor;
        return true;
    }

} }
