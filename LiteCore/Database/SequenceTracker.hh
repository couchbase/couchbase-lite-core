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

        /** Document implementation calls this to register the change with the Notifier. */
        void documentChanged(slice docID, sequence_t);

        sequence_t lastSequence() const         {return _lastSequence;}

    protected:
        /** Tracks a document's current sequence. */
        struct Entry {
            alloc_slice const docID;
            sequence_t sequence {0};
            bool isPlaceholder() const          {return sequence == 0;}

            DatabaseChangeNotifier* const observer {nullptr};
            std::vector<DocChangeNotifier*> documentObservers;

            Entry(slice d, sequence_t s)        :docID(d), sequence(s) { }
            Entry(DatabaseChangeNotifier *o)    :observer(o) { }
        };

        typedef std::list<Entry>::const_iterator const_iterator;

        /** Returns an iterator positioned at the first Entry with sequence after s. */

        /** Returns the oldest Entry. */
        const_iterator begin() const            {return _changes.begin();}

        /** Returns the end of the Entry list. */
        const_iterator end() const              {return _changes.end();}

        const_iterator addPlaceholder(DatabaseChangeNotifier *obs);
        const_iterator addPlaceholderAfter(DatabaseChangeNotifier *obs, sequence_t);
        void removePlaceholder(const_iterator);
        bool hasChangesAfterPlaceholder(const_iterator) const;
        std::vector<const Entry*> catchUpPlaceholder(const_iterator);
        const_iterator addDocChangeNotifier(slice docID, DocChangeNotifier*);
        void removeDocChangeNotifier(const_iterator, DocChangeNotifier*);

    private:
        friend class DatabaseChangeNotifier;
        friend class DocChangeNotifier;
        friend class SequenceTrackerTest;

        const_iterator _since(sequence_t s) const;

        typedef std::list<Entry>::iterator iterator;

        std::mutex _mutex;              //TODO: Use shared_mutex when available (C++17)
        std::list<Entry> _changes;
        std::unordered_map<slice, iterator, fleece::sliceHash> _byDocID;
        sequence_t _lastSequence {0};
        size_t _numPlaceholders {0};
    };


    /** Tracks changes to a single document and calls a client callback. */
    class DocChangeNotifier {
    public:
        typedef std::function<void(DocChangeNotifier&)> Callback;

        DocChangeNotifier(SequenceTracker &tracker, slice docID, Callback cb)
        :_tracker(tracker),
         _docEntry(tracker.addDocChangeNotifier(docID, this)),
         callback(cb)
        { }

        ~DocChangeNotifier() {
            _tracker.removeDocChangeNotifier(_docEntry, this);
        }

        Callback const callback;

        slice docID() const             {return _docEntry->docID;}
        sequence_t sequence() const     {return _docEntry->sequence;}

    protected:
        void notify() {if (callback) callback(*this);}

    private:
        friend class SequenceTracker;
        SequenceTracker &_tracker;
        SequenceTracker::const_iterator const _docEntry;
    };


    /** Tracks changes to a database and calls a client callback. */
    class DatabaseChangeNotifier {
    public:
        /** A callback that will be invoked _once_ when new changes arrive. After that, calling
            `changes` will reset the state so the callback can be called again. */
        typedef std::function<void(DatabaseChangeNotifier&)> Callback;

        DatabaseChangeNotifier(SequenceTracker&, Callback);

        DatabaseChangeNotifier(SequenceTracker&, Callback, sequence_t afterSeq);

        ~DatabaseChangeNotifier();

        Callback const callback;

        /** Returns true if there are new changes, i.e. if `changes` would return a non-empty vector. */
        bool hasChanges() const;

        /** Returns all the changes that have occurred since the last call to `changes` (or since
            construction.) Resets the callback state so it can be called again. */
        std::vector<const SequenceTracker::Entry*> changes();

    protected:
        void notify() {if (callback) callback(*this);}

    private:
        friend class SequenceTracker;

        SequenceTracker &_tracker;
        SequenceTracker::const_iterator const _placeholder;
    };

}
