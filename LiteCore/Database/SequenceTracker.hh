//
// SequenceTracker.hh
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

#pragma once
#include "Base.hh"
#include "Error.hh"
#include "Logging.hh"
#include <list>
#include <unordered_map>
#include <vector>
#include <functional>

namespace c4Internal {
    class Database;
    class Document;
}

namespace litecore {
    class DatabaseChangeNotifier;
    class DocChangeNotifier;

    extern LogDomain ChangesLog; // "Changes"
    

    /** Tracks database & document changes, and notifies listeners.
        It's intended that this be a singleton per database _file_. */
    class SequenceTracker : public Logging {
    public:
        struct Entry;

        SequenceTracker();

        void beginTransaction();
        void endTransaction(bool commit);

        /** Document implementation calls this to register the change with the Notifier. */
        void documentChanged(const alloc_slice &docID,
                             const alloc_slice &revID,
                             sequence_t sequence,
                             uint64_t bodySize);

        /** Document implementation calls this to register the change with the Notifier. */
        void documentPurged(slice docID);

        /** Copy the other tracker's transaction's changes into myself as committed & external */
        void addExternalTransaction(const SequenceTracker &from);

        sequence_t lastSequence() const        {return _lastSequence;}

        /** Tracks a document's current sequence. */
        struct Entry {
            alloc_slice const               docID;
            sequence_t                      sequence {0};

            // Document entry (when docID != nullslice):
            sequence_t                      committedSequence {0};
            alloc_slice                     revID;
            std::vector<DocChangeNotifier*> documentObservers;
            uint32_t                        bodySize;
            bool                            idle     :1;
            bool                            external :1;

            // Placeholder entry (when docID == nullslice):
            DatabaseChangeNotifier* const   databaseObserver {nullptr};

            Entry(const alloc_slice &d, alloc_slice r, sequence_t s, uint32_t bs)
            :docID(d), revID(r), sequence(s), bodySize(bs), idle(false), external(false) {
                DebugAssert(docID != nullslice);
            }
            Entry(DatabaseChangeNotifier *o)
            :databaseObserver(o) { }    // placeholder

            bool isPlaceholder() const          {return docID.buf == nullptr;}
            bool isPurge() const                {return sequence == 0 && !isPlaceholder();}
            bool isIdle() const                 {return idle && !isPlaceholder();}
        };

        struct Change {
            alloc_slice docID;
            alloc_slice revID;
            sequence_t sequence;
            uint32_t bodySize;
        };

#if DEBUG
        /** Writes a string representation for debugging/testing purposes. Format is a list of
            comma-separated entries, inside square brackets. Each entry is either "docid@sequence"
            for a change, or "*" for a placeholder. The entries in an open transaction are
            inside a pair of parentheses.
            The `verbose` flag adds '#' followed by the body size to each entry. */
        std::string dump(bool verbose =false) const;
#endif

        static size_t kMinChangesToKeep;        // exposed for testing purposes only

    protected:
        typedef std::list<Entry>::const_iterator const_iterator;

        bool inTransaction() const              {return _transaction.get() != nullptr;}

        /** Returns the oldest Entry. */
        const_iterator begin() const            {return _changes.begin();}

        /** Returns the end of the Entry list. */
        const_iterator end() const              {return _changes.end();}

        const_iterator addPlaceholderAfter(DatabaseChangeNotifier *obs, sequence_t);
        void removePlaceholder(const_iterator);
        bool hasChangesAfterPlaceholder(const_iterator) const;
        size_t readChanges(const_iterator placeholder,
                           Change changes[], size_t maxChanges,
                           bool &external);
        const_iterator addDocChangeNotifier(slice docID, DocChangeNotifier*);
        void removeDocChangeNotifier(const_iterator, DocChangeNotifier*);
        void removeObsoleteEntries();

    private:
        friend class DatabaseChangeNotifier;
        friend class DocChangeNotifier;
        friend class SequenceTrackerTest;

        void _documentChanged(const alloc_slice &docID,
                              const alloc_slice &revID,
                              sequence_t sequence,
                              uint64_t bodySize);
        const_iterator _since(sequence_t s) const;

        typedef std::list<Entry>::iterator iterator;

        std::list<Entry>                        _changes;
        std::list<Entry>                        _idle;
        std::unordered_map<slice, iterator, fleece::sliceHash> _byDocID;
        sequence_t                              _lastSequence {0};
        size_t                                  _numPlaceholders {0};
        size_t                                  _numDocObservers {0};
        std::unique_ptr<DatabaseChangeNotifier> _transaction;
        sequence_t                              _preTransactionLastSequence;
    };


    /** Tracks changes to a single document and calls a client callback. */
    class DocChangeNotifier {
    public:
        typedef std::function<void(DocChangeNotifier&, slice docID, sequence_t)> Callback;

        DocChangeNotifier(SequenceTracker &t, slice docID, Callback cb);
        ~DocChangeNotifier();

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
    class DatabaseChangeNotifier : public Logging {
    public:
        /** A callback that will be invoked _once_ when new changes arrive. After that, calling
            `readChanges` will reset the state so the callback can be called again. */
        typedef std::function<void(DatabaseChangeNotifier&)> Callback;

        DatabaseChangeNotifier(SequenceTracker&, Callback, sequence_t afterSeq =UINT64_MAX);

        ~DatabaseChangeNotifier();

        SequenceTracker &tracker;
        Callback const callback;

        /** Returns true if there are new changes, i.e. if `changes` would return a non-empty vector. */
        bool hasChanges() const {
            return tracker.hasChangesAfterPlaceholder(_placeholder);
        }

        /** Returns changes that have occurred since the last call to `changes` (or since
            construction.) Resets the callback state so it can be called again. */
        size_t readChanges(SequenceTracker::Change changes[], size_t maxChanges, bool &external);

    protected:
        void notify();

    private:
        friend class SequenceTracker;

        SequenceTracker::const_iterator const _placeholder;
    };

}
