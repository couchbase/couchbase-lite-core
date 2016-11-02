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
*/


namespace litecore {
    using namespace std;


    static const size_t kMinChangesToKeep = 100;


    void SequenceTracker::documentChanged(slice docID, sequence_t sequence) {
        lock_guard<mutex> writeLock(_mutex);

        Assert(sequence > _lastSequence);
        _lastSequence = sequence;
        
        bool listChanged = true;
        auto i = _byDocID.find(docID);
        if (i != _byDocID.end()) {
            // Update existing entry's sequence, and move it to the end of the list:
            i->second->sequence = sequence;
            if (next(i->second) != _changes.end())
                _changes.splice(_changes.end(), _changes, i->second);
            else
                listChanged = false;
            // Notify document notifiers:
            for (auto docNotifier : i->second->documentObservers)
                docNotifier->notify();
        } else {
            // or create a new entry at the end:
            _changes.emplace_back(docID, sequence);
            iterator change = prev(_changes.end());
            _byDocID[change->docID] = change;
        }

        if (listChanged) {
            // Any placeholders right before this change were up to date, should be notified:
            auto ph = next(_changes.rbegin());      // iterating _backwards_
            while (ph != _changes.rend() && ph->isPlaceholder()) {
                auto nextph = ph;
                ++nextph; // precompute next pos, in case 'ph' moves itself during the callback
                ph->observer->notify();
                ph = nextph;
            }
        }
    }


    SequenceTracker::const_iterator
    SequenceTracker::_since(sequence_t sinceSeq) const {
        return lower_bound(_changes.cbegin(), _changes.cend(),
                           sinceSeq + 1,
                           [](const Entry &c, sequence_t s) {return c.sequence < s;} );
        //OPT: This is O(n) since _changes is a linked list. I don't expect it to be called often enough for this to be a problem, but I could be wrong.
        //OPT: Might be more efficient to search backwards, if sinceSeq is usually recent?
    }


    SequenceTracker::const_iterator
    SequenceTracker::addPlaceholder(DatabaseChangeNotifier *obs) {
        lock_guard<mutex> writeLock(_mutex);

        Assert(obs);
        ++_numPlaceholders;
        _changes.emplace_back(obs);
        return prev(_changes.end());
    }

    SequenceTracker::const_iterator
    SequenceTracker::addPlaceholderAfter(DatabaseChangeNotifier *obs, sequence_t seq) {
        lock_guard<mutex> writeLock(_mutex);

        Assert(obs);
        ++_numPlaceholders;
        return _changes.emplace(_since(seq), obs);
    }

    void SequenceTracker::removePlaceholder(const_iterator placeholder) {
        lock_guard<mutex> writeLock(_mutex);

        _changes.erase(placeholder);
        --_numPlaceholders;
    }


    bool SequenceTracker::hasChangesAfterPlaceholder(const_iterator placeholder) const {
        for (auto i = next(placeholder); i != _changes.end(); ++i) {
            if (!i->isPlaceholder())
                return true;
        }
        return false;
    }

    vector<const SequenceTracker::Entry*>
    SequenceTracker::catchUpPlaceholder(const_iterator placeholder) {
        lock_guard<mutex> readLock(const_cast<SequenceTracker*>(this)->_mutex);

        vector<const SequenceTracker::Entry*> changes;
        for (auto i = next(placeholder); i != _changes.end(); ++i) {
            if (!i->isPlaceholder())
                changes.push_back(&*i);
        }

        _changes.splice(_changes.end(), _changes, placeholder);

        // Any changes before the first placeholder aren't going to be seen, so remove them:
        while (_changes.size() - _numPlaceholders > kMinChangesToKeep
                    && !_changes.front().isPlaceholder()) {
            _byDocID.erase(_changes.front().docID);
            _changes.erase(_changes.begin());
            //FIX: Shouldn't erase it if its documentObservers is non-empty
        }

        return changes;
    }


    SequenceTracker::const_iterator
    SequenceTracker::addDocChangeNotifier(slice docID, DocChangeNotifier* notifier) {
        lock_guard<mutex> writeLock(_mutex);

        auto i = _byDocID.find(docID);
        if (i != _byDocID.end()) {
            i->second->documentObservers.push_back(notifier);
            return i->second;
        } else {
            //TODO: ??? What if the doc isn't registered yet?
            error::_throw(error::Unimplemented);
        }
    }


    void SequenceTracker::removeDocChangeNotifier(const_iterator entry, DocChangeNotifier* notifier) {
        lock_guard<mutex> writeLock(_mutex);

        auto &observers = const_cast<vector<DocChangeNotifier*>&>(entry->documentObservers);
        auto i = find(observers.begin(), observers.end(), notifier);
        Assert(i != observers.end());
        observers.erase(i);
    }


#pragma mark - DATABASE CHANGE NOTIFIER:


    DatabaseChangeNotifier::DatabaseChangeNotifier(SequenceTracker &tracker, Callback cb)
    :_tracker(tracker),
     callback(cb),
     _placeholder(tracker.addPlaceholder(this))
    { }


    DatabaseChangeNotifier::DatabaseChangeNotifier(SequenceTracker &tracker, Callback cb, sequence_t afterSeq)
    :_tracker(tracker),
     callback(cb),
     _placeholder(tracker.addPlaceholderAfter(this, afterSeq))
    { }


    DatabaseChangeNotifier::~DatabaseChangeNotifier() {
        _tracker.removePlaceholder(_placeholder);
    }


    bool DatabaseChangeNotifier::hasChanges() const {
        return _tracker.hasChangesAfterPlaceholder(_placeholder);
    }


    vector<const SequenceTracker::Entry*> DatabaseChangeNotifier::changes() {
        return _tracker.catchUpPlaceholder(_placeholder);
    }

}
