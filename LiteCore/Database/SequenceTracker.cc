//
//  SequenceTracker.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "SequenceTracker.hh"
#include "Document.hh"
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


    static const size_t kMinChangesToKeep = 100;


    void SequenceTracker::beginTransaction() {
        auto notifier = new DatabaseChangeNotifier(*this, nullptr);
        Assert(!inTransaction());
        _transaction.reset(notifier);
        _preTransactionLastSequence = _lastSequence;
    }


    void SequenceTracker::endTransaction(bool commit) {
        // Get a list of Entrys for all docs changed in the transaction:
        Assert(inTransaction());

        if (commit) {
            // Bump their committedSequences:
            for (auto entry = next(_transaction->_placeholder); entry != _changes.end(); ++entry) {
                if (!entry->isPlaceholder()) {
                    const_cast<Entry&>(*entry).committedSequence = entry->sequence;
                }
            }

        } else {
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
                                          uint64_t bodySize) {
        Assert(inTransaction());
        Assert(sequence > _lastSequence);
        _lastSequence = sequence;
        _documentChanged(docID, revID, sequence, bodySize);
    }


    void SequenceTracker::_documentChanged(const alloc_slice &docID,
                                           const alloc_slice &revID,
                                           sequence_t sequence,
                                           uint64_t bodySize)
    {
        auto shortBodySize = (uint32_t)min(bodySize, (uint64_t)UINT32_MAX);
        bool listChanged = true;
        Entry *entry;
        auto i = _byDocID.find(docID);
        if (i != _byDocID.end()) {
            // Move existing entry to the end of the list:
            entry = &*i->second;
            if (entry->isIdle() && !hasDBChangeNotifiers()) {
                listChanged = false;
            } else {
                if (entry->isIdle()) {
                    _changes.splice(_changes.end(), _idle, i->second);
                    entry->idle = false;
                } else if (next(i->second) != _changes.end())
                    _changes.splice(_changes.end(), _changes, i->second);
                else
                    listChanged = false;
            }
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

        if (listChanged) {
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
        for (auto e = next(other._transaction->_placeholder); e != other._changes.end(); ++e) {
            _lastSequence = e->sequence;
            _documentChanged(e->docID, e->revID, e->sequence, e->bodySize);
        }
    }


    SequenceTracker::const_iterator
    SequenceTracker::_since(sequence_t sinceSeq) const {
        if (sinceSeq >= _lastSequence) {
            return _changes.cend();
        } else {
            return lower_bound(_changes.cbegin(), _changes.cend(),
                               sinceSeq + 1,
                               [](const Entry &c, sequence_t s) {return c.sequence < s;} );
            //OPT: This is O(n) since _changes is a linked list. I don't expect it to be called often enough for this to be a problem, but I could be wrong.
            //OPT: Might be more efficient to search backwards, if sinceSeq is usually recent?
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
                Change change = {i->docID, i->revID, i->sequence, i->bodySize};
                changes[n++] = change;
            }
            ++i;
        }
        if (n > 0) {
            _changes.splice(i, _changes, placeholder);
            // (It would be nice to call removeObsoleteEntries now, but it could free the entries
            // that own the docID slices I'm about to return.)
        }
        return n;
   }


    void SequenceTracker::removeObsoleteEntries() {
        if (inTransaction())
            return;
        // Any changes before the first placeholder aren't going to be seen, so remove them:
        while (_changes.size() - _numPlaceholders > kMinChangesToKeep
                    && !_changes.front().isPlaceholder()) {
            _byDocID.erase(_changes.front().docID);
            _changes.erase(_changes.begin());
            //FIX: Shouldn't erase it if its documentObservers is non-empty
        }
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
        return entry;
    }


    void SequenceTracker::removeDocChangeNotifier(const_iterator entry, DocChangeNotifier* notifier) {
        auto &observers = const_cast<vector<DocChangeNotifier*>&>(entry->documentObservers);
        auto i = find(observers.begin(), observers.end(), notifier);
        Assert(i != observers.end());
        observers.erase(i);
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


#pragma mark - DATABASE CHANGE NOTIFIER:


    DatabaseChangeNotifier::DatabaseChangeNotifier(SequenceTracker &t, Callback cb, sequence_t afterSeq)
    :tracker(t),
     callback(cb),
     _placeholder(tracker.addPlaceholderAfter(this, afterSeq))
    { }

}
