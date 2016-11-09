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
 On abort: Iterate over all changes since that placeholder and call documentChanged, with the old sequence number. (Where do we get it from?) This will notify all observers that the doc has reverted back.
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
        auto committedChanges = changesSincePlaceholder(_transaction->_placeholder);

        if (!commit)
            _lastSequence = _preTransactionLastSequence;

        // Bump or revert their committedSequences:
        for (auto change : committedChanges) {
            if (commit)
                const_cast<Entry*>(change)->committedSequence = change->sequence;
            else
                _documentChanged(change->docID, change->committedSequence);
        }

        if (commit && _commitCallback)
            _commitCallback(*this, committedChanges);

        _transaction.reset();
        removeObsoleteEntries();
    }


    void SequenceTracker::documentChanged(slice docID, sequence_t sequence) {
        Assert(sequence > _lastSequence);
        _lastSequence = sequence;
        _documentChanged(docID, sequence);
    }


    void SequenceTracker::_documentChanged(slice docID, sequence_t sequence) {
        bool listChanged = true;
        auto i = _byDocID.find(docID);
        if (i != _byDocID.end()) {
            // Move existing entry to the end of the list:
            Entry *entry = &*i->second;
            if (entry->isIdle() && !hasDBChangeNotifiers()) {
                listChanged = false;
            } else {
                if (entry->isIdle())
                    _changes.splice(_changes.end(), _idle, i->second);
                else if (next(i->second) != _changes.end())
                    _changes.splice(_changes.end(), _changes, i->second);
                else
                    listChanged = false;
            }
            // Update its sequence:
            entry->sequence = sequence;
            if (!inTransaction())
                entry->committedSequence = sequence;
            // Notify document notifiers:
            for (auto docNotifier : entry->documentObservers)
                docNotifier->notify(entry);
        } else {
            // or create a new entry at the end:
            if (!hasDBChangeNotifiers())
                return; // ... unless there are no notifiers in place to care
            _changes.emplace_back(docID, sequence);
            iterator change = prev(_changes.end());
            _byDocID[change->docID] = change;
            if (!inTransaction())
                change->committedSequence = sequence;
        }

        if (listChanged) {
            // Any placeholders right before this change were up to date, should be notified:
            auto ph = next(_changes.rbegin());      // iterating _backwards_
            while (ph != _changes.rend() && ph->isPlaceholder()) {
                auto nextph = ph;
                ++nextph; // precompute next pos, in case 'ph' moves itself during the callback
                if (ph->databaseObserver)
                    ph->databaseObserver->notify();
                ph = nextph;
            }
        }
    }


    void SequenceTracker::documentsChanged(const vector<const Entry*>& entries) {
        for (auto change : entries)
            documentChanged(change->docID, change->committedSequence);
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


    vector<const SequenceTracker::Entry*>
    SequenceTracker::changesSincePlaceholder(const_iterator placeholder) {
        vector<const SequenceTracker::Entry*> changes;
        for (auto i = next(placeholder); i != _changes.end(); ++i) {
            if (!i->isPlaceholder())
                changes.push_back(&*i);
        }
        return changes;
    }


    size_t SequenceTracker::readChanges(const_iterator placeholder,
                                        slice docIDs[], size_t numIDs)
    {
        size_t n = 0;
        auto i = next(placeholder);
        while (i != _changes.end() && n < numIDs) {
            if (!i->isPlaceholder())
                docIDs[n++] = i->docID;
            ++i;
        }
        if (n > 0) {
            _changes.splice(i, _changes, placeholder);
            // (It would be nice to call removeObsoleteEntries now, but it could free the entries
            // that own the docID slices I'm about to return.)
        }
        return n;
   }


    void SequenceTracker::catchUpPlaceholder(const_iterator placeholder) {
        _changes.splice(_changes.end(), _changes, placeholder);
        removeObsoleteEntries();
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
            entry = _idle.emplace(_idle.end(), docID, 0);
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
            _idle.erase(entry);
        }
    }


#pragma mark - DATABASE CHANGE NOTIFIER:


    DatabaseChangeNotifier::DatabaseChangeNotifier(SequenceTracker &tracker, Callback cb, sequence_t afterSeq)
    :_tracker(tracker),
     callback(cb),
     _placeholder(tracker.addPlaceholderAfter(this, afterSeq))
    { }

}
