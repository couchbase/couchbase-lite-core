//
// SequenceTracker.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SequenceTracker.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "Error.hh"
#include <algorithm>
#include <sstream>
#include <utility>

/* THEORY OF OPERATION:
 
Placeholders are interspersed with documents (represented by Entry objects) in the _changes list.
    Pl1 -> A -> Z -> Pl2 -> B -> F
if document A is changed, its Entry's sequence is updated and it moves to the end:
    Pl1 -> Z -> Pl2 -> B -> F -> A
DatabaseChangeNotifier's readChanges method moves the placeholder forward, adding any Entrys
passed over to the resulting changes[] array until it reaches the end or the array is full.
           Z -> Pl2 -> B -> F -> A -> Pl1       (and readChanges results in [Z, B, F, A])
Any document Entry objects before the first placeholder can now be removed:
                Pl2 -> B -> F -> A -> Pl1
After a document changes and its Entry moves to the end, if the item(s) _directly_ before the Entry
are placeholders, their notifiers post notifications.
Here document F changed, and notifier 1 posts a notification:
                Pl2 -> B -> A -> Pl1 -> F
Then document A changes, but no notifications are sent:
                Pl2 -> B -> Pl1 -> F -> A

Transactions:
 On begin:
    * A special placeholder (`_transaction`) is added at the end of the list.
 After the DB transaction commits:
    * The Database object is responsible for finding all other SequenceTrackers on the same file
      and calling their `addExternalTransaction` method to notify them (see below.)
    * For each Entry after `_transaction`:
        - Set the Entry's `committedSequence` equal to its `sequence`.
    * Remove the `_transaction` placeholder.
 After the DB transaction aborts:
    * For each Entry after `_transaction`:
        - Call _documentChanged, with the Entry's old committed sequence number;
          this generates a fake change representing the reversion of uncommitted changes.
    * Remove the `_transaction` placeholder.
 When notified that another connection's SequenceTracker is committing changes:
    * For each Entry after the other's `_transaction`:
        - Call _documentChanged to add an equivalent Entry. Set the Entry's `external` flag so
          clients know this change was not made by this database connection.
*/


namespace litecore {
    using namespace std;


    size_t SequenceTracker::kMinChangesToKeep = 100;

    LogDomain ChangesLog("Changes", LogLevel::Warning);

    /** Tracks a document's current sequence. */
    struct SequenceTracker::Entry {
        alloc_slice const docID;
        sequence_t        sequence{0};

        // Document entry (when docID != nullslice):
        sequence_t                              committedSequence{0};
        alloc_slice                             revID;
        mutable std::vector<DocChangeNotifier*> documentObservers;
        uint32_t                                bodySize;
        RevisionFlags                           flags;
        bool                                    idle : 1;
        bool                                    external : 1;

        // Placeholder entry (when docID == nullslice):
        CollectionChangeNotifier* const databaseObserver{nullptr};

        Entry(alloc_slice d, alloc_slice r, sequence_t s, uint32_t bs, RevisionFlags flags)
            : docID(std::move(d))
            , revID(std::move(r))
            , sequence(s)
            , bodySize(bs)
            , flags(flags)
            , idle(false)
            , external(false) {
            DebugAssert(docID != nullslice);
        }

        explicit Entry(const alloc_slice& d) : Entry(d, nullslice, 0_seq, 0, {}) {}

        // Disable warning about uninitialized fields. It seems intended behaviour for this constructor
        // NOLINTBEGIN(cppcoreguidelines-pro-type-member-init)
        explicit Entry(CollectionChangeNotifier* o NONNULL) : databaseObserver(o) {}  // placeholder

        // NOLINTEND(cppcoreguidelines-pro-type-member-init)

        bool isPlaceholder() const { return docID.buf == nullptr; }

        bool isPurge() const { return sequence == 0_seq && !isPlaceholder(); }

        bool isIdle() const { return idle && !isPlaceholder(); }

        Entry(const Entry&)            = delete;
        Entry& operator=(const Entry&) = delete;
    };

    SequenceTracker::SequenceTracker(slice name) : Logging(ChangesLog), _name(name) {}

#if defined(_MSC_VER) && _MSC_VER < 1920
    SequenceTracker::SequenceTracker(SequenceTracker&& other) noexcept
        : Logging(ChangesLog)
        , _name(std::move(other._name))
        , _changes(std::move(other._changes))
        , _idle(std::move(other._idle))
        , _byDocID(std::move(other._byDocID))
        , _lastSequence(other._lastSequence)
        , _numPlaceholders(other._numPlaceholders)
        , _numDocObservers(other._numDocObservers)
        , _transaction(std::move(other._transaction))
        , _preTransactionLastSequence(other._preTransactionLastSequence) {}
#else
    // Another compiler bug that I assume Microsoft is not going to fix in 2017
    SequenceTracker::SequenceTracker(SequenceTracker&&) noexcept = default;
#endif


    SequenceTracker::~SequenceTracker() = default;

    void SequenceTracker::beginTransaction() {
        Assert(!inTransaction());

        logInfo("begin transaction at #%" PRIu64, (uint64_t)_lastSequence);
        _transaction                = make_unique<CollectionChangeNotifier>(this, nullptr);
        _preTransactionLastSequence = _lastSequence;
    }

    bool SequenceTracker::changedDuringTransaction() const {
        Assert(inTransaction());
        if ( _lastSequence > _preTransactionLastSequence ) return true;
        for ( auto entry = next(_transaction->_placeholder); entry != _changes.end(); ++entry )
            if ( !entry->isPlaceholder() ) return true;
        return false;
    }

    void SequenceTracker::endTransaction(bool commit) {
        Assert(inTransaction());

        bool housekeeping = false;
        if ( commit ) {
            logInfo("commit: sequences #%" PRIu64 " -- #%" PRIu64, (uint64_t)_preTransactionLastSequence + 1,
                    (uint64_t)_lastSequence);
            // Bump their committedSequences:
            for ( auto entry = next(_transaction->_placeholder); entry != _changes.end(); ++entry ) {
                if ( !entry->isPlaceholder() ) {
                    const_cast<Entry&>(*entry).committedSequence = entry->sequence;
                    housekeeping                                 = true;
                }
            }

        } else {
            logInfo("abort: from seq #%" PRIu64 " back to #%" PRIu64, (uint64_t)_lastSequence,
                    (uint64_t)_preTransactionLastSequence);
            _lastSequence = _preTransactionLastSequence;

            // Revert their committedSequences:
            auto           lastEntry = std::prev(_changes.end());
            auto           nextEntry = _transaction->_placeholder;
            const_iterator entry;
            do {
                entry     = nextEntry;
                nextEntry = next(entry);
                if ( !entry->isPlaceholder() ) {
                    // moves entry!
                    _documentChanged(entry->docID, entry->revID, entry->committedSequence, entry->bodySize,
                                     entry->flags);
                }
            } while ( entry != lastEntry );
            housekeeping = true;
        }

        _transaction.reset();
        if ( housekeeping ) removeObsoleteEntries();
    }

    void SequenceTracker::documentChanged(const alloc_slice& docID, const alloc_slice& revID, sequence_t sequence,
                                          uint64_t bodySize, RevisionFlags flags) {
        Assert(inTransaction());
        Assert(docID && revID && sequence > _lastSequence);

        _lastSequence = sequence;
        _documentChanged(docID, revID, sequence, bodySize, flags);
    }

    void SequenceTracker::documentPurged(slice docID) {
        Assert(docID);
        Assert(inTransaction());

        _documentChanged(alloc_slice(docID), {}, 0_seq, 0, {});
    }

    void SequenceTracker::_documentChanged(const alloc_slice& docID, const alloc_slice& revID, sequence_t sequence,
                                           uint64_t bodySize, RevisionFlags flags) {
        logDebug("documentChanged('%.*s', %.*s, %llu, size=%llu, flags=%hhx", SPLAT(docID), SPLAT(revID), sequence,
                 bodySize, flags);
        auto   shortBodySize = (uint32_t)min(bodySize, (uint64_t)UINT32_MAX);
        bool   listChanged   = true;
        Entry* entry;
        auto   i = _byDocID.find(docID);
        if ( i != _byDocID.end() ) {
            // Move existing entry to the end of the list:
            entry = &*i->second;
            if ( entry->isIdle() ) {
                _changes.splice(_changes.end(), _idle, i->second);
                entry->idle = false;
            } else if ( next(i->second) != _changes.end() )
                _changes.splice(_changes.end(), _changes, i->second);
            else
                listChanged = false;  // it was already at the end
            // Update its revID & sequence:
            entry->revID    = revID;
            entry->sequence = sequence;
            entry->bodySize = shortBodySize;
            entry->flags    = flags;
            entry->external = false;
        } else {
            // or create a new entry at the end:
            _changes.emplace_back(docID, revID, sequence, shortBodySize, flags);
            auto change             = prev(_changes.end());
            _byDocID[change->docID] = change;
            entry                   = &*change;
        }

        if ( !inTransaction() ) {
            entry->committedSequence = sequence;
            entry->external          = true;  // it must have come from addExternalTransaction()
        }

        // Notify document notifiers:
        for ( auto docNotifier : entry->documentObservers ) docNotifier->notify(entry);

        if ( listChanged && _numPlaceholders > 0 ) {
            // Any placeholders right before this change were up to date, should be notified:
            bool notified = false;
            auto ph       = next(_changes.rbegin());  // iterating _backwards_, skipping latest
            while ( ph != _changes.rend() && ph->isPlaceholder() ) {
                auto nextph = ph;
                ++nextph;  // precompute next pos, in case 'ph' moves itself during the callback
                if ( ph->databaseObserver ) {
                    ph->databaseObserver->notify();
                    notified = true;
                }
                ph = nextph;
            }
            if ( notified ) removeObsoleteEntries();
        }
    }

    void SequenceTracker::addExternalTransaction(const SequenceTracker& other) {
        Assert(!inTransaction());
        Assert(other.inTransaction());
        if ( !_changes.empty() || _numDocObservers > 0 ) {
            logInfo("addExternalTransaction from %s", other.loggingIdentifier().c_str());
            auto end = other._changes.end();
            for ( auto e = next(other._transaction->_placeholder); e != end; ++e ) {
                if ( !e->isPlaceholder() ) {
                    if ( e->sequence != 0_seq ) {
                        Assert(e->sequence > _lastSequence);
                        _lastSequence = e->sequence;
                    }
                    _documentChanged(e->docID, e->revID, e->sequence, e->bodySize, e->flags);
                }
            }
            removeObsoleteEntries();
        }
    }

    SequenceTracker::const_iterator SequenceTracker::begin() const { return _changes.begin(); }

    SequenceTracker::const_iterator SequenceTracker::end() const { return _changes.end(); }

    SequenceTracker::const_iterator SequenceTracker::_since(sequence_t sinceSeq) const {
        if ( sinceSeq >= _lastSequence ) {
            return _changes.cend();
        } else {
            // Scan back till we find a document entry with sequence less than sinceSeq
            // (but not a purge); then back up one:
            auto result = _changes.crbegin();
            for ( auto i = _changes.crbegin(); i != _changes.crend(); ++i ) {
                if ( i->sequence > sinceSeq || i->isPurge() ) result = i;
                else if ( !i->isPlaceholder() )
                    break;
            }
            return prev(result.base());  // (convert `result` to regular fwd iterator)
        }
    }

    slice SequenceTracker::_docIDAt(sequence_t seq) const { return _since(seq)->docID; }

    SequenceTracker::const_iterator SequenceTracker::addPlaceholderAfter(CollectionChangeNotifier* obs,
                                                                         sequence_t                seq) {
        Assert(obs);
        ++_numPlaceholders;
        return _changes.emplace(_since(seq), obs);
    }

    void SequenceTracker::removePlaceholder(const_iterator placeholder) {
        _changes.erase(placeholder);
        --_numPlaceholders;
        removeObsoleteEntries();
    }

    bool SequenceTracker::hasChangesAfterPlaceholder(const_iterator placeholder) const {
        for ( auto i = next(placeholder); i != _changes.end(); ++i ) {
            if ( !i->isPlaceholder() ) return true;
        }
        return false;
    }

    size_t SequenceTracker::readChanges(const_iterator placeholder, Change changes[], size_t maxChanges,
                                        bool& external) {
        external = false;
        size_t n = 0;
        auto   i = next(placeholder);
        while ( i != _changes.end() && n < maxChanges ) {
            if ( !i->isPlaceholder() ) {
                // During the loop, collect only changes with the same value for `external`:
                if ( n == 0 ) external = i->external;
                else if ( i->external != external )
                    break;
                if ( changes ) changes[n++] = Change{i->docID, i->revID, i->sequence, i->bodySize, i->flags};
            }
            ++i;
        }
        if ( n > 0 ) {
            // Move `placeholder` to just before `i`:
            _changes.splice(i, _changes, placeholder);
            removeObsoleteEntries();
        }
        return n;
    }

    void SequenceTracker::removeObsoleteEntries() {
        if ( inTransaction() ) return;
        // Any changes before the first placeholder aren't going to be seen, so remove them:
        size_t nRemoved = 0;
        while ( _changes.size() > kMinChangesToKeep + _numPlaceholders && !_changes.front().isPlaceholder() ) {
            auto& entry = _changes.front();
            if ( entry.documentObservers.empty() ) {
                // Remove entry entirely if it has no observers
                _byDocID.erase(entry.docID);
                _changes.erase(_changes.begin());
            } else {
                // Move entry to idle list if it has observers
                _idle.splice(_idle.end(), _changes, _changes.begin());
                entry.idle = true;
            }
            ++nRemoved;
        }
        logVerbose("Removed %zu old entries (%zu left; idle has %zd, byDocID has %zu)", nRemoved, _changes.size(),
                   _idle.size(), _byDocID.size());
    }

    SequenceTracker::const_iterator SequenceTracker::addDocChangeNotifier(slice docID, DocChangeNotifier* notifier) {
        Assert(docID);
        iterator entry;
        // Find the entry for the document:
        auto i = _byDocID.find(docID);
        if ( i != _byDocID.end() ) {
            entry = i->second;
        } else {
            // Document isn't known yet; create an entry and put it in the _idle list
            entry                  = _idle.emplace(_idle.end(), alloc_slice(docID));
            entry->idle            = true;
            _byDocID[entry->docID] = entry;
        }
        entry->documentObservers.push_back(notifier);
        ++_numDocObservers;
        return entry;
    }

    void SequenceTracker::removeDocChangeNotifier(const_iterator entry, DocChangeNotifier* notifier) {
        auto& observers = entry->documentObservers;
        auto  i         = find(observers.begin(), observers.end(), notifier);
        Assert(i != observers.end(), "unknown DocChangeNotifier");
        observers.erase(i);
        --_numDocObservers;
        if ( observers.empty() && entry->isIdle() ) {
            _byDocID.erase(entry->docID);
            Assert(!_idle.empty());
            _idle.erase(entry);
        }
    }


#if DEBUG
    string SequenceTracker::dump(bool verbose) const {
        stringstream s;
        s << "[";
        bool first = true;
        for ( auto i = begin(); i != end(); ++i ) {
            if ( first ) first = false;
            else
                s << ", ";
            if ( !i->isPlaceholder() ) {
                s << (string)i->docID << "@" << uint64_t(i->sequence);
                if ( verbose && i->flags != RevisionFlags::None ) s << '#' << hex << int(i->flags) << dec;
                if ( verbose ) s << '+' << i->bodySize;
                if ( i->external ) s << "'";
            } else if ( _transaction && i == _transaction->_placeholder ) {
                s << "(";
                first = true;
            } else {
                s << "*";
            }
        }
        if ( _transaction ) s << ")";
        s << "]";
        return s.str();
    }
#endif


#pragma mark - DOC CHANGE NOTIFIER:

    DocChangeNotifier::DocChangeNotifier(SequenceTracker* t, slice docID, Callback cb)
        : tracker(t), _docEntry(tracker->addDocChangeNotifier(docID, this)), callback(std::move(cb)) {
        t->_logVerbose("Added doc change notifier %p for '%.*s'", this, SPLAT(docID));
    }

    DocChangeNotifier::~DocChangeNotifier() {
        if ( tracker ) {
            tracker->_logVerbose("Removing doc change notifier %p from '%.*s'", this, SPLAT(_docEntry->docID));
            tracker->removeDocChangeNotifier(_docEntry, this);
        }
    }

    slice DocChangeNotifier::docID() const { return _docEntry->docID; }

    sequence_t DocChangeNotifier::sequence() const { return _docEntry->sequence; }

    void DocChangeNotifier::notify(const SequenceTracker::Entry* entry) noexcept {
        if ( callback ) callback(*this, entry->docID, entry->sequence);
    }

#pragma mark - DATABASE CHANGE NOTIFIER:

    CollectionChangeNotifier::CollectionChangeNotifier(SequenceTracker* t, Callback cb, sequence_t afterSeq)
        : Logging(ChangesLog)
        , tracker(t)
        , callback(std::move(cb))
        , _placeholder(tracker->addPlaceholderAfter(this, afterSeq)) {
        if ( callback ) logInfo("Created, starting after #%" PRIu64, (uint64_t)afterSeq);
    }

    CollectionChangeNotifier::~CollectionChangeNotifier() {
        if ( callback ) logInfo("Deleting");

        if ( tracker ) { tracker->removePlaceholder(_placeholder); }
    }

    void CollectionChangeNotifier::notify() noexcept {
        if ( callback ) {
            logInfo("posting notification");
            callback(*this);
        }
    }

    size_t CollectionChangeNotifier::readChanges(SequenceTracker::Change changes[], size_t maxChanges, bool& external) {
        size_t n = tracker->readChanges(_placeholder, changes, maxChanges, external);
        logInfo("readChanges(%zu) -> %zu changes", maxChanges, n);
        return n;
    }


}  // namespace litecore
