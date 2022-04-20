//
// SequenceTracker.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "SequenceTracker.hh"
#include "Document.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include <algorithm>
#include <sstream>


/*
Placeholders are interspersed with the document change objects in the list.
    Pl1 -> A -> Z -> Pl2 -> B -> F
if document A is changed, its sequence is updated and it moves to the end:
    Pl1 -> Z -> Pl2 -> B -> F -> A
After a notifier iterates through the changes following its placeholder,
it moves its placeholder to the end:
           Z -> Pl2 -> B -> F -> A -> Pl1
Any document change items before the first placeholder can be removed:
                Pl2 -> B -> F -> A -> Pl1
After a document changes, if the item(s) directly before its change object are placeholders,
their notifiers post notifications. Here document F changed, and notifier 1 posts a notification:
                Pl2 -> B -> A -> Pl1 -> F

Transactions:
 When a transaction begins, a placeholder is added at the end of the list.
 On commit: Generate a list of all changes since that placeholder, and broadcast to all other databases open on this file. They add those changes to their SequenceTrackers.
 On abort: Iterate over all changes since that placeholder and call documentChanged, with the old committed sequence number. This will notify all observers that the doc has reverted back.
*/


namespace litecore {
    using namespace std;


    size_t SequenceTracker::kMinChangesToKeep = 100;

    LogDomain ChangesLog("Changes", LogLevel::Warning);


    SequenceTracker::SequenceTracker()
    :Logging(ChangesLog)
    { }


    void SequenceTracker::beginTransaction() {
        logInfo("begin transaction at #%" PRIu64, _lastSequence);
        auto notifier = new DatabaseChangeNotifier(*this, nullptr);
        Assert(!inTransaction());
        _transaction.reset(notifier);
        _preTransactionLastSequence = _lastSequence;
    }


    void SequenceTracker::endTransaction(bool commit) {
        Assert(inTransaction());

        if (commit) {
            logInfo("commit: sequences #%" PRIu64 " -- #%" PRIu64, _preTransactionLastSequence, _lastSequence);
            // Bump their committedSequences:
            for (auto entry = next(_transaction->_placeholder); entry != _changes.end(); ++entry) {
                if (!entry->isPlaceholder()) {
                    const_cast<Entry&>(*entry).committedSequence = entry->sequence;
                }
            }

        } else {
            logInfo("abort: from seq #%" PRIu64 " back to #%" PRIu64, _lastSequence, _preTransactionLastSequence);
            _lastSequence = _preTransactionLastSequence;

            // Revert their committedSequences:
            const_iterator lastEntry = prev(_changes.end());
            const_iterator nextEntry = _transaction->_placeholder;
            const_iterator entry;
            do {
                entry = nextEntry;
                nextEntry = next(entry);
                if (!entry->isPlaceholder()) {
                    // moves entry!
                    _documentChanged(entry->docID, entry->revID,
                                     entry->committedSequence, entry->bodySize);
                }
            } while (entry != lastEntry);
        }

        _transaction.reset();
        removeObsoleteEntries();
    }


    void SequenceTracker::documentChanged(const alloc_slice &docID,
                                          const alloc_slice &revID,
                                          sequence_t sequence,
                                          uint64_t bodySize)
    {
        Assert(docID && revID && sequence > _lastSequence);
        Assert(inTransaction());
        _lastSequence = sequence;
        _documentChanged(docID, revID, sequence, bodySize);
    }


    void SequenceTracker::documentPurged(slice docID) {
        Assert(docID);
        Assert(inTransaction());
        _documentChanged(alloc_slice(docID), {}, 0, 0);
    }


    void SequenceTracker::_documentChanged(const alloc_slice &docID,
                                           const alloc_slice &revID,
                                           sequence_t sequence,
                                           uint64_t bodySize)
    {
        logDebug("documentChanged('%.*s', %.*s, %llu, size=%llu",
                 SPLAT(docID), SPLAT(revID), sequence, bodySize);
        auto shortBodySize = (uint32_t)min(bodySize, (uint64_t)UINT32_MAX);
        bool listChanged = true;
        Entry *entry;
        auto i = _byDocID.find(docID);
        if (i != _byDocID.end()) {
            // Move existing entry to the end of the list:
            entry = &*i->second;
            if (entry->isIdle()) {
                _changes.splice(_changes.end(), _idle, i->second);
                entry->idle = false;
            } else if (next(i->second) != _changes.end())
                _changes.splice(_changes.end(), _changes, i->second);
            else
                listChanged = false;  // it was already at the end
            // Update its revID & sequence:
            entry->revID = revID;
            entry->sequence = sequence;
            entry->bodySize = shortBodySize;
        } else {
            // or create a new entry at the end:
            _changes.emplace_back(docID, revID, sequence, shortBodySize);
            iterator change = prev(_changes.end());
            _byDocID[change->docID] = change;
            entry = &*change;
        }

        if (!inTransaction()) {
            entry->committedSequence = sequence;
            entry->external = true; // it must have come from addExternalTransaction()
        }

        // Notify document notifiers:
        for (auto docNotifier : entry->documentObservers)
            docNotifier->notify(entry);

        if (listChanged && _numPlaceholders > 0) {
            // Any placeholders right before this change were up to date, should be notified:
            bool notified = false;
            auto ph = next(_changes.rbegin());      // iterating _backwards_, skipping latest
            while (ph != _changes.rend() && ph->isPlaceholder()) {
                auto nextph = ph;
                ++nextph; // precompute next pos, in case 'ph' moves itself during the callback
                if (ph->databaseObserver) {
                    ph->databaseObserver->notify();
                    notified = true;
                }
                ph = nextph;
            }
            if (notified)
                removeObsoleteEntries();
        }
    }


    void SequenceTracker::addExternalTransaction(const SequenceTracker &other) {
        Assert(!inTransaction());
        Assert(other.inTransaction());
        if (!_changes.empty() || _numDocObservers > 0) {
            logInfo("addExternalTransaction from %s", other.loggingIdentifier().c_str());
            for (auto e = next(other._transaction->_placeholder); e != other._changes.end(); ++e) {
                if (!e->isPlaceholder()) {
                    _lastSequence = e->sequence;
                    _documentChanged(e->docID, e->revID, e->sequence, e->bodySize);
                }
            }
            removeObsoleteEntries();
        }
    }


    SequenceTracker::const_iterator
    SequenceTracker::_since(sequence_t sinceSeq) const {
        if (sinceSeq >= _lastSequence) {
            return _changes.cend();
        } else {
            // Scan back till we find a document entry with sequence less than sinceSeq
            // (but not a purge); then back up one:
            auto result = _changes.crbegin();
            for (auto i = _changes.crbegin(); i != _changes.crend(); ++i) {
                if (i->sequence > sinceSeq || i->isPurge())
                    result = i;
                else if (!i->isPlaceholder())
                    break;
            }
            return prev(result.base());      // (convert `result` to regular fwd iterator)
        }
    }


    SequenceTracker::const_iterator
    SequenceTracker::addPlaceholderAfter(DatabaseChangeNotifier *obs, sequence_t seq) {
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
        for (auto i = next(placeholder); i != _changes.end(); ++i) {
            if (!i->isPlaceholder())
                return true;
        }
        return false;
    }


    size_t SequenceTracker::readChanges(const_iterator placeholder,
                                        Change changes[], size_t maxChanges,
                                        bool &external)
    {
        external = false;
        size_t n = 0;
        auto i = next(placeholder);
        while (i != _changes.end() && n < maxChanges) {
            if (!i->isPlaceholder()) {
                if (n == 0)
                    external = i->external;
                else if (i->external != external)
                    break;
                if (changes)
                    changes[n++] = Change{i->docID, i->revID, i->sequence, i->bodySize};
            }
            ++i;
        }
        if (n > 0) {
            _changes.splice(i, _changes, placeholder);
            removeObsoleteEntries();
        }
        return n;
   }


    void SequenceTracker::removeObsoleteEntries() {
        if (inTransaction())
            return;
        // Any changes before the first placeholder aren't going to be seen, so remove them:
        size_t nRemoved = 0;
        while (_changes.size() > kMinChangesToKeep + _numPlaceholders
                    && !_changes.front().isPlaceholder()) {
            auto &entry = _changes.front();
            if (entry.documentObservers.empty()) {
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
        logVerbose("Removed %zu old entries (%zu left; idle has %zd, byDocID has %zu)",
                   nRemoved, _changes.size(), _idle.size(), _byDocID.size());
    }


    SequenceTracker::const_iterator
    SequenceTracker::addDocChangeNotifier(slice docID, DocChangeNotifier* notifier) {
        iterator entry;
        // Find the entry for the document:
        auto i = _byDocID.find(docID);
        if (i != _byDocID.end()) {
            entry = i->second;
        } else {
            // Document isn't known yet; create an entry and put it in the _idle list
            entry = _idle.emplace(_idle.end(), alloc_slice(docID), alloc_slice(), 0, 0);
            entry->idle = true;
            _byDocID[entry->docID] = entry;
        }
        entry->documentObservers.push_back(notifier);
        ++_numDocObservers;
        return entry;
    }


    void SequenceTracker::removeDocChangeNotifier(const_iterator entry, DocChangeNotifier* notifier) {
        auto &observers = const_cast<vector<DocChangeNotifier*>&>(entry->documentObservers);
        auto i = find(observers.begin(), observers.end(), notifier);
        Assert(i != observers.end());
        observers.erase(i);
        --_numDocObservers;
        if (observers.empty() && entry->isIdle()) {
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
        for (auto i = begin(); i != end(); ++i) {
            if (first)
                first = false;
            else
                s << ", ";
            if (!i->isPlaceholder()) {
                s << (string)i->docID << "@" << i->sequence;
                if (verbose)
                    s << '#' << i->bodySize;
                if (i->external)
                    s << "'";
            } else if (_transaction && i == _transaction->_placeholder) {
                s << "(";
                first = true;
            } else {
                s << "*";
            }
        }
        if (_transaction)
            s << ")";
        s << "]";
        return s.str();
    }
#endif


#pragma mark - DOC CHANGE NOTIFIER:


    DocChangeNotifier::DocChangeNotifier(SequenceTracker &t, slice docID, Callback cb)
    :tracker(t),
    _docEntry(tracker.addDocChangeNotifier(docID, this)),
    callback(cb)
    {
        t._logVerbose("Added doc change notifier %p for '%.*s'", this, SPLAT(docID));
    }

    DocChangeNotifier::~DocChangeNotifier() {
        tracker._logVerbose("Removing doc change notifier %p from '%.*s'", this, SPLAT(_docEntry->docID));
        tracker.removeDocChangeNotifier(_docEntry, this);
    }




#pragma mark - DATABASE CHANGE NOTIFIER:


    DatabaseChangeNotifier::DatabaseChangeNotifier(SequenceTracker &t, Callback cb, sequence_t afterSeq)
    :Logging(ChangesLog)
    ,tracker(t)
    ,callback(cb)
    ,_placeholder(tracker.addPlaceholderAfter(this, afterSeq))
    {
        if (callback)
            logInfo("Created, starting after #%" PRIu64, afterSeq);
    }


    DatabaseChangeNotifier::~DatabaseChangeNotifier() {
        if (callback)
            logInfo("Deleting");
        tracker.removePlaceholder(_placeholder);
    }


    void DatabaseChangeNotifier::notify() {
        if (callback) {
            logInfo("posting notification");
            callback(*this);
        }
    }


    size_t DatabaseChangeNotifier::readChanges(SequenceTracker::Change changes[],
                                               size_t maxChanges,
                                               bool &external) {
        size_t n = tracker.readChanges(_placeholder, changes, maxChanges, external);
        logInfo("readChanges(%zu) -> %zu changes", maxChanges, n);
        return n;
    }


}
