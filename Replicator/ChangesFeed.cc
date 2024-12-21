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
#include "StringUtil.hh"
#include "c4DocEnumerator.hh"
#include "c4Observer.hh"
#include "fleece/Fleece.hh"
#include <cinttypes>
#include <cstdint>

using namespace std;
using namespace fleece;

namespace litecore::repl {


    ChangesFeed::ChangesFeed(Delegate& delegate, const Options* options, DBAccess& db, Checkpointer* checkpointer)
        : Logging(SyncLog)
        , _delegate(delegate)
        , _options(options)
        , _db(db)
        , _checkpointer(checkpointer)
        , _skipDeleted(_options->skipDeleted()) {
        DebugAssert(_checkpointer);

        // JIM: This breaks tons of encapsulation, and should be reworked
        _collectionIndex =
                (CollectionIndex)_options->collectionSpecToIndex().at(_checkpointer->collection()->getSpec());
        _continuous = _options->push(_collectionIndex) == kC4Continuous;
        filterByDocIDs(_options->docIDs(_collectionIndex));
    }

    ChangesFeed::~ChangesFeed() = default;

    void ChangesFeed::filterByDocIDs(Array docIDs) {
        if ( !docIDs ) return;
        DocIDSet combined(new unordered_set<string>);
        combined->reserve(docIDs.count());
        for ( Array::iterator i(docIDs); i; ++i ) {
            string docID = i.value().asstring();
            if ( !docID.empty() && (!_docIDs || _docIDs->find(docID) != _docIDs->end()) )
                combined->insert(std::move(docID));
        }
        _docIDs = std::move(combined);
        if ( !_options->isActive() ) logInfo("Peer requested filtering to %zu docIDs", _docIDs->size());
    }

    // Gets the next batch of changes from the DB. Will respond by calling gotChanges.
    ChangesFeed::Changes ChangesFeed::getMoreChanges(unsigned limit) {
        Assert(limit > 0);

        if ( _continuous && !_changeObserver ) {
            // Start the observer immediately, before querying historical changes, to avoid any
            // gaps between the history and notifications. But do not set `_notifyOnChanges` yet.
            logVerbose("Starting DB observer");
            _changeObserver = C4DatabaseObserver::create(_checkpointer->collection(),
                                                         [this](C4DatabaseObserver*) { this->_dbChanged(); });
        }

        Changes changes       = {};
        changes.firstSequence = _maxSequence + 1;
        if ( _caughtUp && _continuous ) getObservedChanges(changes, limit);
        else
            getHistoricalChanges(changes, limit);
        changes.lastSequence = _maxSequence;

        if ( _options->isActive() && changes.lastSequence >= changes.firstSequence ) {
            _checkpointer->addPendingSequences(changes.revs, changes.firstSequence, changes.lastSequence);
        }
        return changes;
    }

    void ChangesFeed::getHistoricalChanges(Changes& changes, unsigned limit) {
        logVerbose("Reading up to %u local changes since #%" PRIu64, limit, (uint64_t)_maxSequence);

        // Run a by-sequence enumerator to find the changed docs:
        C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
        // TBD: pushFilter should be collection-aware.
        if ( !_getForeignAncestors && !_options->pushFilter(_collectionIndex) ) options.flags &= ~kC4IncludeBodies;
        if ( !_skipDeleted ) options.flags |= kC4IncludeDeleted;
        if ( _db.usingVersionVectors() ) options.flags |= kC4IncludeRevHistory;

        try {
            _db.useLocked([&](C4Database* db) {
                Assert(db == _checkpointer->collection()->getDatabase());
                options.flags |= kC4RevIDGlobalForm;
                C4DocEnumerator e(_checkpointer->collection(), _maxSequence, options);
                changes.revs.reserve(limit);
                while ( e.next() && limit > 0 ) {
                    C4DocumentInfo info = e.documentInfo();
                    auto           rev  = makeRevToSend(info, &e);
                    if ( rev ) {
                        changes.revs.push_back(rev);
                        --limit;
                    }
                }
            });
        } catch ( ... ) { changes.err = C4Error::fromCurrentException(); }

        if ( limit > 0 && !_caughtUp ) {
            // Couldn't get as many changes as asked for, so I've caught up with the DB.
            _caughtUp = true;
        }
        changes.askAgain = !_caughtUp || _continuous;
    }

    void ChangesFeed::getObservedChanges(Changes& changes, unsigned limit) {
        logVerbose("Asking DB observer for %u new changes since sequence #%" PRIu64 " ...", limit,
                   (uint64_t)_maxSequence);
        static constexpr uint32_t  kMaxChanges = 100;
        C4DatabaseObserver::Change c4changes[kMaxChanges];
        C4CollectionObservation    nextObservation;
        auto const                 startingMaxSequence = _maxSequence;

        _notifyOnChanges = true;

        while ( limit > 0 ) {
            nextObservation   = _changeObserver->getChanges(c4changes, min(limit, kMaxChanges));
            uint32_t nChanges = nextObservation.numChanges;
            if ( nChanges == 0 ) break;

            if ( !nextObservation.external && !_echoLocalChanges ) {
                logDebug("Observed %u of my own db changes #%" PRIu64 " ... #%" PRIu64 " (ignoring)", nChanges,
                         static_cast<uint64_t>(c4changes[0].sequence),
                         static_cast<uint64_t>(c4changes[nChanges - 1].sequence));
                _maxSequence = c4changes[nChanges - 1].sequence;
                continue;  // ignore changes I made myself
            }
            logVerbose("Observed %u db changes #%" PRIu64 " ... #%" PRIu64, nChanges, (uint64_t)c4changes[0].sequence,
                       (uint64_t)c4changes[nChanges - 1].sequence);

            // Copy the changes into a vector of RevToSend:
            C4DatabaseObserver::Change* c4change        = &c4changes[0];
            auto                        oldChangesCount = changes.revs.size();
            for ( uint32_t i = 0; i < nChanges; ++i, ++c4change ) {
                // The sequence of a purge change is 0. Therefore the following statement
                // will effectively, beside other effects, skip the changes due to Purge.
                if ( c4change->sequence <= startingMaxSequence ) continue;
                C4DocumentInfo info = {};
                info.flags          = c4change->flags;
                info.docID          = c4change->docID;
                info.revID          = c4change->revID;
                info.sequence       = c4change->sequence;
                info.bodySize       = c4change->bodySize;
                // Note: we send tombstones even if the original getChanges() call specified
                // skipDeletions. This is intentional; skipDeletions applies only to the initial
                // dump of existing docs, not to 'live' changes.
                if ( auto rev = makeRevToSend(info, nullptr); rev ) {
                    // It's possible but unlikely to get the same docID in successive calls to
                    // c4dbobs_getChanges, if it changes in between calls. Remove the older:
                    for ( size_t j = 0; j < oldChangesCount; ++j ) {
                        if ( changes.revs[j]->docID == c4change->docID ) {
                            changes.revs.erase(changes.revs.begin() + narrow_cast<long>(j));
                            ++limit;
                            break;
                        }
                    }
                    changes.revs.push_back(rev);
                    --limit;
                }
            }
        }

        if ( changes.revs.empty() ) logInfo("No new observed changes...");
        else if ( limit > 0 )
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
        if ( _notifyOnChanges.exchange(false) )  // test-and-clear
            _delegate.dbHasNewChanges();
    }

    // Common subroutine of _getChanges and dbChanged that adds a document to a list of Revs.
    // It does some quick tests, and if those pass creates a RevToSend and passes it on to the
    // other shouldPushRev, which does more expensive checks.
    Retained<RevToSend> ChangesFeed::makeRevToSend(C4DocumentInfo& info, C4DocEnumerator* e) {
        _maxSequence = info.sequence;
        if ( info.expiration > C4Timestamp::None && info.expiration < c4_now() ) {
            logVerbose("'%.*s' is expired; not pushing it", SPLAT(info.docID));
            return nullptr;  // skip rev: expired
            // skip rev: checkpoint says we already pushed it before
        } else if ( (_options->isActive() && _checkpointer->isSequenceCompleted(info.sequence)) ||
                    // skip rev: not in list of docIDs
                    (_docIDs != nullptr && _docIDs->find(slice(info.docID).asString()) == _docIDs->end()) ) {
            return nullptr;
        } else {
            auto rev = make_retained<RevToSend>(info, _checkpointer->collection()->getSpec(),
                                                _options->collectionCallbackContext(_collectionIndex));
            return shouldPushRev(rev, e) ? rev : nullptr;
        }
    }

    bool ChangesFeed::shouldPushRev(RevToSend* rev) const { return shouldPushRev(rev, nullptr); }

    // This is called both by revToSend, and by Pusher::doneWithRev.
    bool ChangesFeed::shouldPushRev(RevToSend* rev, C4DocEnumerator* e) const {
        bool needRemoteRevID = _getForeignAncestors && !rev->remoteAncestorRevID && _isCheckpointValid;
        if ( needRemoteRevID || _options->pushFilter(_collectionIndex) ) {
            C4Error              error;
            Retained<C4Document> doc;
            try {
                _db.useLocked([&](C4Database* db) {
                    if ( e ) doc = e->getDocument();
                    else
                        doc = _checkpointer->collection()->getDocument(
                                rev->docID, true, (needRemoteRevID ? kDocGetAll : kDocGetCurrentRev));
                    if ( !doc ) error = C4Error::make(LiteCoreDomain, kC4ErrorNotFound);
                });
            } catch ( ... ) { error = C4Error::fromCurrentException(); }
            if ( !doc ) {
                _delegate.failedToGetChange(rev, error, false);
                return false;  // fail the rev: error getting doc
            }

            if ( !C4Document::equalRevIDs(doc->getSelectedRevIDGlobalForm(), rev->revID) )
                return false;  // skip rev: there's a newer one already

            if ( needRemoteRevID ) {
                // For proposeChanges, find the nearest foreign ancestor of the current rev:
                if ( !getRemoteRevID(rev, doc) ) return false;  // skip or fail rev: it's already on the peer
            }
            if ( _options->pushFilter(_collectionIndex) ) {
                // If there's a push filter, ask it whether to push the doc:
                if ( !_options->pushFilter(_collectionIndex)(_checkpointer->collection()->getSpec(), doc->docID(),
                                                             doc->selectedRev().revID, doc->selectedRev().flags,
                                                             doc->getProperties(),
                                                             _options->collectionCallbackContext(_collectionIndex)) ) {
                    logVerbose("Doc '%.*s' rejected by push filter", SPLAT(doc->docID()));
                    return false;  // skip rev: rejected by push filter
                }
            }
        }
        return true;
    }

    // Overridden by ReplicatorChangesFeed
    bool ChangesFeed::getRemoteRevID(RevToSend* rev, C4Document* doc) const { return true; }

#pragma mark - REPLICATOR CHANGES FEED:

    ReplicatorChangesFeed::ReplicatorChangesFeed(Delegate& delegate, const Options* options, DBAccess& db,
                                                 Checkpointer* cp)
        : ChangesFeed(delegate, options, db, cp)  // DBAccess is a subclass of access_lock<C4Database*>
        , _usingVersionVectors(db.usingVersionVectors()) {}

    // Assigns rev->remoteAncestorRevID based on the document.
    // Returns false to reject the document if the remote is equal to or newer than this rev.
    bool ReplicatorChangesFeed::getRemoteRevID(RevToSend* rev, C4Document* doc) const {
        // For proposeChanges, find the nearest foreign ancestor of the current rev:
        auto& dbAccess = (DBAccess&)_db;
        Assert(dbAccess.remoteDBID());
        alloc_slice foreignAncestor = dbAccess.getDocRemoteAncestor(doc);
        logDebug("remoteRevID of '%.*s' is %.*s", SPLAT(doc->docID()), SPLAT(foreignAncestor));
        if ( foreignAncestor == slice(doc->revID()) ) return false;  // skip this rev: it's already on the peer
        if ( foreignAncestor && !_usingVersionVectors
             && C4Document::getRevIDGeneration(foreignAncestor) >= C4Document::getRevIDGeneration(doc->revID()) ) {
            if ( !_options->isActive() ) {
                C4Error error = C4Error::make(WebSocketDomain, 409, "conflicts with newer server revision"_sl);
                _delegate.failedToGetChange(rev, error, false);
            }
            return false;  // skip or fail rev: there's a newer one on the peer
        }
        rev->remoteAncestorRevID = foreignAncestor;
        return true;
    }

    ChangesFeed::Changes ReplicatorChangesFeed::getMoreChanges(unsigned limit) {
        if ( _getForeignAncestors ) ((DBAccess&)_db).markRevsSyncedNow();  // make sure foreign ancestors are up to date
        return ChangesFeed::getMoreChanges(limit);
    }

}  // namespace litecore::repl
