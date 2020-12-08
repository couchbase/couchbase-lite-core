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
#include "Checkpointer.hh"
#include "ReplicatorOptions.hh"
#include "DBAccess.hh"
#include "ReplicatorTuning.hh"
#include "StringUtil.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4DocEnumerator.h"
#include "c4Document+Fleece.h"
#include "c4DocEnumerator.h"
#include "c4Observer.h"
#include "fleece/Fleece.hh"
#include <cinttypes>

using namespace std;
using namespace fleece;

namespace litecore { namespace repl {


    ChangesFeed::ChangesFeed(Delegate &delegate, Options &options,
                             access_lock<C4Database*> &db, Checkpointer *checkpointer)
    :Logging(SyncLog)
    ,_delegate(delegate)
    ,_options(options)
    ,_db(db)
    ,_checkpointer(checkpointer)
    ,_continuous(_options.push == kC4Continuous)
    ,_passive(_options.push <= kC4Passive)
    ,_skipDeleted(_options.skipDeleted())
    {
        filterByDocIDs(_options.docIDs());
    }


    string ChangesFeed::loggingClassName() const  {
        string className = Logging::loggingClassName();
        if (_passive)
            toLowercase(className);
        return className;
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
    ChangesFeed::Changes ChangesFeed::getMoreChanges(unsigned limit) {
        Assert(limit > 0);

        if (_continuous && !_changeObserver) {
            // Start the observer immediately, before querying historical changes, to avoid any
            // gaps between the history and notifications. But do not set `_notifyOnChanges` yet.
            logVerbose("Starting DB observer");
            _db.use([&](C4Database* db) {
                _changeObserver = c4dbobs_create(db,
                                                 [](C4DatabaseObserver* observer, void *context) {
                                                     ((ChangesFeed*)context)->_dbChanged();
                                                 },
                                                 this);
            });
        }

        Changes changes = {};
        changes.firstSequence = _maxSequence + 1;
        if (_caughtUp && _continuous)
            getObservedChanges(changes, limit);
        else
            getHistoricalChanges(changes, limit);
        changes.lastSequence = _maxSequence;

        if (!_passive && _checkpointer && changes.lastSequence >= changes.firstSequence) {
            _checkpointer->addPendingSequences(changes.revs,
                                               changes.firstSequence, changes.lastSequence);
        }
        return changes;
    }


    void ChangesFeed::getHistoricalChanges(Changes &changes, unsigned limit) {
        logVerbose("Reading up to %u local changes since #%" PRIu64, limit, _maxSequence);

        // Run a by-sequence enumerator to find the changed docs:
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        if (!_getForeignAncestors && !_options.pushFilter)
            options.flags &= ~kC4IncludeBodies;
        if (!_skipDeleted)
            options.flags |= kC4IncludeDeleted;

        _db.use([&](C4Database* db) {
            c4::ref<C4DocEnumerator> e = c4db_enumerateChanges(db, _maxSequence, &options, &changes.err);
            if (e) {
                changes.revs.reserve(limit);
                while (c4enum_next(e, &changes.err) && limit > 0) {
                    C4DocumentInfo info;
                    c4enum_getDocumentInfo(e, &info);
                    auto rev = makeRevToSend(info, e, db);
                    if (rev) {
                        changes.revs.push_back(rev);
                        --limit;
                    }
                }
            }
        });

        if (limit > 0 && !_caughtUp) {
            // Couldn't get as many changes as asked for, so I've caught up with the DB.
            _caughtUp = true;
        }
        changes.askAgain = !_caughtUp || _continuous;
    }


    void ChangesFeed::getObservedChanges(Changes &changes, unsigned limit) {
        logVerbose("Asking DB observer for %u new changes since sequence #%" PRIu64 " ...",
                   limit, _maxSequence);
        static constexpr uint32_t kMaxChanges = 100;
        C4DatabaseChange c4changes[kMaxChanges];
        bool ext;
        uint32_t nChanges;
        auto const startingMaxSequence = _maxSequence;

        _notifyOnChanges = true;

        while (limit > 0) {
            nChanges = c4dbobs_getChanges(_changeObserver, c4changes, min(limit,kMaxChanges), &ext);
            if (nChanges == 0)
                break;

            if (!ext && !_echoLocalChanges) {
                logDebug("Observed %u of my own db changes #%" PRIu64 " ... #%" PRIu64 " (ignoring)",
                         nChanges, c4changes[0].sequence, c4changes[nChanges-1].sequence);
                _maxSequence = c4changes[nChanges-1].sequence;
                continue;     // ignore changes I made myself
            }
            logVerbose("Observed %u db changes #%" PRIu64 " ... #%" PRIu64,
                       nChanges, c4changes[0].sequence, c4changes[nChanges-1].sequence);

            // Copy the changes into a vector of RevToSend:
            C4DatabaseChange *c4change = c4changes;
            _db.use([&](C4Database *db) {
                auto oldChangesCount = changes.revs.size();
                for (uint32_t i = 0; i < nChanges; ++i, ++c4change) {
                    if (c4change->sequence <= startingMaxSequence)
                        continue;
                    C4DocumentInfo info {0, c4change->docID, c4change->revID,
                                         c4change->sequence, c4change->bodySize};
                    // Note: we send tombstones even if the original getChanges() call specified
                    // skipDeletions. This is intentional; skipDeletions applies only to the initial
                    // dump of existing docs, not to 'live' changes.
                    if (auto rev = makeRevToSend(info, nullptr, db); rev) {
                        // It's possible but unlikely to get the same docID in successive calls to
                        // c4dbobs_getChanges, if it changes in between calls. Remove the older:
                        for (size_t j = 0; j < oldChangesCount; ++j) {
                            if(changes.revs[j]->docID == c4change->docID) {
                                changes.revs.erase(changes.revs.begin() + j);
                                ++limit;
                                break;
                            }
                        }
                        changes.revs.push_back(rev);
                        --limit;
                    }
                }
            });

            c4dbobs_releaseChanges(c4changes, nChanges);
        }

        if (changes.revs.empty())
            logInfo("No new observed changes...");
        else if (limit > 0)
            logVerbose("Read all observed changes; awaiting more...");
        else {
            // I returned a full list of changes, so I know the caller will call me again when
            // it's ready for more. Therefore, it's not necessary to notify it.
            _notifyOnChanges = false;
            changes.askAgain = true;
        }
    }


    // Callback from the C4DatabaseObserver when the database has changed
    // **This is called on an arbitrary thread!**
    void ChangesFeed::_dbChanged() {
        logVerbose("Database changed! [notify=%d]", _notifyOnChanges.load());
        if (_notifyOnChanges.exchange(false))  // test-and-clear
            _delegate.dbHasNewChanges();
    }


    // Common subroutine of _getChanges and dbChanged that adds a document to a list of Revs.
    // It does some quick tests, and if those pass creates a RevToSend and passes it on to the
    // other shouldPushRev, which does more expensive checks.
    Retained<RevToSend> ChangesFeed::makeRevToSend(C4DocumentInfo &info, C4DocEnumerator *e, C4Database *db)
    {
        _maxSequence = info.sequence;
        if (info.expiration > 0 && info.expiration < c4_now()) {
            logVerbose("'%.*s' is expired; not pushing it", SPLAT(info.docID));
            return nullptr;             // skip rev: expired
        } else if (!_passive && _checkpointer && _checkpointer->isSequenceCompleted(info.sequence)) {
            return nullptr;             // skip rev: checkpoint says we already pushed it before
        } else if (_docIDs != nullptr
                    && _docIDs->find(slice(info.docID).asString()) == _docIDs->end()) {
            return nullptr;             // skip rev: not in list of docIDs
        } else {
            auto rev = retained(new RevToSend(info));
            return shouldPushRev(rev, e, db) ? rev : nullptr;
        }
    }


    bool ChangesFeed::shouldPushRev(RevToSend *rev) const {
        return _db.use<bool>([&](C4Database *db) {
            return shouldPushRev(rev, nullptr, db);
        });
    }


    // This is called both by revToSend, and by Pusher::doneWithRev.
    bool ChangesFeed::shouldPushRev(RevToSend *rev, C4DocEnumerator *e, C4Database *db) const {
        bool needRemoteRevID = _getForeignAncestors && !rev->remoteAncestorRevID
                                                    && _isCheckpointValid;
        if (needRemoteRevID || _options.pushFilter) {
            c4::ref<C4Document> doc;
            C4Error error;
            doc = e ? c4enum_getDocument(e, &error) : c4doc_get(db, rev->docID, true, &error);
            if (!doc) {
                _delegate.failedToGetChange(rev, error, false);
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


    // Overridden by ReplicatorChangesFeed
    bool ChangesFeed::getRemoteRevID(RevToSend *rev, C4Document *doc) const {
        return true;
    }


#pragma mark - REPLICATOR CHANGES FEED:


    ReplicatorChangesFeed::ReplicatorChangesFeed(Delegate &delegate, Options &options, DBAccess &db, Checkpointer *cp)
    :ChangesFeed(delegate, options, db, cp)     // DBAccess is a subclass of access_lock<C4Database*>
    { }


    // Assigns rev->remoteAncestorRevID based on the document.
    // Returns false to reject the document if the remote is equal to or newer than this rev.
    bool ReplicatorChangesFeed::getRemoteRevID(RevToSend *rev, C4Document *doc) const {
        // For proposeChanges, find the nearest foreign ancestor of the current rev:
        auto &dbAccess = (DBAccess&)_db;
        Assert(dbAccess.remoteDBID());
        alloc_slice foreignAncestor = dbAccess.getDocRemoteAncestor(doc);
        logDebug("remoteRevID of '%.*s' is %.*s", SPLAT(doc->docID), SPLAT(foreignAncestor));
        if (foreignAncestor == slice(doc->revID))
            return false;   // skip this rev: it's already on the peer
        if (foreignAncestor
                    && c4rev_getGeneration(foreignAncestor) >= c4rev_getGeneration(doc->revID)) {
            if (_options.pull <= kC4Passive) {
                C4Error error = c4error_make(WebSocketDomain, 409,
                                     "conflicts with newer server revision"_sl);
                _delegate.failedToGetChange(rev, error, false);
            }
            return false;    // skip or fail rev: there's a newer one on the peer
        }
        rev->remoteAncestorRevID = foreignAncestor;
        return true;
    }


    ChangesFeed::Changes ReplicatorChangesFeed::getMoreChanges(unsigned limit) {
        if (_getForeignAncestors)
            ((DBAccess&)_db).markRevsSyncedNow();  // make sure foreign ancestors are up to date
        return ChangesFeed::getMoreChanges(limit);
    }

} }
