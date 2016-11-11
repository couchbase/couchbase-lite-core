//
//  SequenceTracker.hh
//  LiteCore
//
//  Created by Jens Alfke on 10/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#pragma once
#include "Base.hh"
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace c4Internal {
    class Database;
    class Document;
}

namespace litecore {
    class DatabaseChangeNotifier;
    class DocChangeNotifier;


    /** Tracks database & document changes, and notifies listeners.
        It's intended that this be a singleton per database _file_. */
    class SequenceTracker {
    public:
        struct Entry;

        SequenceTracker() { }

        /** Multithreaded clients can use this to synchronize access to the tracker. */
        std::mutex& mutex()                     {return _mutex;}

        void beginTransaction();
        void endTransaction(bool commit);

        /** Document implementation calls this to register the change with the Notifier. */
        void documentChanged(const alloc_slice &docID, sequence_t);

        void documentsChanged(const std::vector<const Entry*>&);

        /** Copy the other tracker's transaction's changes into myself as committed & external */
        void addExternalTransaction(const SequenceTracker &from);

        sequence_t lastSequence() const         {return _lastSequence;}

        /** Tracks a document's current sequence. */
        struct Entry {
            sequence_t                      sequence {0};

            // Document entry (when sequence != 0):
            sequence_t                      committedSequence {0};
            alloc_slice const               docID;
            std::vector<DocChangeNotifier*> documentObservers;
            bool                            idle     :1;
            bool                            external :1;

            // Placeholder entry (when sequence == 0):
            DatabaseChangeNotifier* const   databaseObserver {nullptr};

            Entry(const alloc_slice &d, sequence_t s)
            :docID(d), sequence(s), idle(false), external(false) { }
            Entry(DatabaseChangeNotifier *o)
            :databaseObserver(o) { }    // placeholder

            bool isPlaceholder() const          {return docID.buf == nullptr;}
            bool isIdle() const                 {return idle && !isPlaceholder();}
        };

#if DEBUG
        /** Writes a string representation for debugging/testing purposes. Format is a list of
            comma-separated entries, inside square brackets. Each entry is either "docid@sequence"
            for a change, or "*" for a placeholder. The entries in an open transaction are
            inside a pair of parentheses. */
        std::string dump() const;
#endif

    protected:
        typedef std::list<Entry>::const_iterator const_iterator;

        bool inTransaction() const              {return _transaction.get() != nullptr;}

        bool hasDBChangeNotifiers() const {
            return _numPlaceholders - (int)inTransaction() > 0;
        }

        /** Returns the oldest Entry. */
        const_iterator begin() const            {return _changes.begin();}

        /** Returns the end of the Entry list. */
        const_iterator end() const              {return _changes.end();}

        const_iterator addPlaceholderAfter(DatabaseChangeNotifier *obs, sequence_t);
        void removePlaceholder(const_iterator);
        bool hasChangesAfterPlaceholder(const_iterator) const;
        size_t readChanges(const_iterator placeholder,
                           slice docIDs[], size_t numIDs,
                           bool &external);
        std::vector<const Entry*> changesSincePlaceholder(const_iterator);
        void catchUpPlaceholder(const_iterator);
        const_iterator addDocChangeNotifier(slice docID, DocChangeNotifier*);
        void removeDocChangeNotifier(const_iterator, DocChangeNotifier*);
        void removeObsoleteEntries();

    private:
        friend class DatabaseChangeNotifier;
        friend class DocChangeNotifier;
        friend class SequenceTrackerTest;

        void _documentChanged(const alloc_slice &docID, sequence_t);
        const_iterator _since(sequence_t s) const;

        typedef std::list<Entry>::iterator iterator;

        std::list<Entry>                        _changes;
        std::list<Entry>                        _idle;
        std::unordered_map<slice, iterator, fleece::sliceHash> _byDocID;
        sequence_t                              _lastSequence {0};
        size_t                                  _numPlaceholders {0};
        std::unique_ptr<DatabaseChangeNotifier> _transaction;
        sequence_t                              _preTransactionLastSequence;
        std::mutex                              _mutex;
    };


    /** Tracks changes to a single document and calls a client callback. */
    class DocChangeNotifier {
    public:
        typedef std::function<void(DocChangeNotifier&, slice docID, sequence_t)> Callback;

        DocChangeNotifier(SequenceTracker &t, slice docID, Callback cb)
        :tracker(t),
         _docEntry(tracker.addDocChangeNotifier(docID, this)),
         callback(cb)
        { }

        ~DocChangeNotifier() {
            tracker.removeDocChangeNotifier(_docEntry, this);
        }

        SequenceTracker &tracker;
        Callback const callback;

        slice docID() const             {return _docEntry->docID;}
        sequence_t sequence() const     {return _docEntry->sequence;}

    protected:
        void notify(const SequenceTracker::Entry* entry) {
            if (callback) callback(*this, entry->docID, entry->sequence);
        }

    private:
        friend class SequenceTracker;
        SequenceTracker::const_iterator const _docEntry;
    };


    /** Tracks changes to a database and calls a client callback. */
    class DatabaseChangeNotifier {
    public:
        /** A callback that will be invoked _once_ when new changes arrive. After that, calling
            `changes` will reset the state so the callback can be called again. */
        typedef std::function<void(DatabaseChangeNotifier&)> Callback;

        DatabaseChangeNotifier(SequenceTracker&, Callback, sequence_t afterSeq =UINT64_MAX);

        ~DatabaseChangeNotifier() {
            tracker.removePlaceholder(_placeholder);
        }

        SequenceTracker &tracker;
        Callback const callback;

        /** Returns true if there are new changes, i.e. if `changes` would return a non-empty vector. */
        bool hasChanges() const {
            return tracker.hasChangesAfterPlaceholder(_placeholder);
        }

        /** Returns changes that have occurred since the last call to `changes` (or since
            construction.) Resets the callback state so it can be called again. */
        size_t readChanges(slice docIDs[], size_t maxDocIDs, bool &external) {
            return tracker.readChanges(_placeholder, docIDs, maxDocIDs, external);
        }

    protected:
        void notify() {if (callback) callback(*this);}

    private:
        friend class SequenceTracker;

        SequenceTracker::const_iterator const _placeholder;
    };

}
