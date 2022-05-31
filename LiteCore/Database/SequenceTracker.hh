//
// SequenceTracker.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "Error.hh"
#include "Logging.hh"
#include <list>
#include <unordered_map>
#include <functional>

namespace litecore {
    class CollectionChangeNotifier;
    class DocChangeNotifier;

    
    extern LogDomain ChangesLog; // "Changes"
    

    /** Tracks collection & document changes, and notifies listeners. */
    class SequenceTracker : public Logging {
    public:
        enum class RevisionFlags : uint8_t { None = 0 };

        SequenceTracker(slice name);
        ~SequenceTracker();
        SequenceTracker(SequenceTracker&&) noexcept;

        slice name() const                      {return _name;}

        /** Call this as soon as the database begins a transaction. */
        void beginTransaction();

        bool changedDuringTransaction() const;

        /** Call this after the database commits or aborts a transaction.
            But before this, you must call \ref addExternalTransaction on all other
            SequenceTrackers on the same database file. */
        void endTransaction(bool commit);

        /** Registers that a document has been changed. Must be called within a transaction.
            This may call change notifier callbacks. */
        void documentChanged(const alloc_slice &docID,
                             const alloc_slice &revID,
                             sequence_t sequence,
                             uint64_t bodySize,
                             RevisionFlags flags);

        /** Registers that the document has been purged. Must be called within a transaction.
            This may call change notifier callbacks. */
        void documentPurged(slice docID);

        /** Copies the other tracker's transaction's changes into myself as committed & external.
            This may call change notifier callbacks.
            This tracker MUST NOT be in a transaction.
            The other tracker MUST be in a transaction.
            The database MUST have just committed its transaction. */
        void addExternalTransaction(const SequenceTracker &from);

        /** The last sequence number seen. */
        sequence_t lastSequence() const        {return _lastSequence;}

        /** A change to a document, as returned from \ref CollectionChangeNotifier::readChanges.
            This struct MUST be identical to the public C4CollectionObserver::Change,
            and have the same layout as C4CollectionChange. */
        struct Change {
            alloc_slice docID;      ///< Document ID
            alloc_slice revID;      ///< Revision ID (ASCII form)
            sequence_t sequence;    ///< Sequence number, or 0 for a purge
            uint32_t bodySize;      ///< Size in bytes of the document body
            RevisionFlags flags;    ///< Revision's flags
        };

#if DEBUG
        /** Writes a string representation for debugging/testing purposes. Format is a list of
            comma-separated entries, inside square brackets. Each entry is either "docid@sequence"
            for a change, or "*" for a placeholder. The entries in an open transaction are
            inside a pair of parentheses.
            The `verbose` flag adds '#' followed by the body size to each entry. */
        std::string dump(bool verbose =false) const;
#endif

    protected:
        struct Entry;
        using iterator = std::list<Entry>::iterator;
        using const_iterator = std::list<Entry>::const_iterator;

        static size_t kMinChangesToKeep;        // exposed for testing purposes only

        bool inTransaction() const              {return _transaction.get() != nullptr;}

        /** Returns the oldest Entry. */
        const_iterator begin() const;

        /** Returns the end of the Entry list. */
        const_iterator end() const;

        const_iterator addPlaceholderAfter(CollectionChangeNotifier *obs NONNULL, sequence_t);
        void removePlaceholder(const_iterator);
        bool hasChangesAfterPlaceholder(const_iterator) const;
        size_t readChanges(const_iterator placeholder,
                           Change changes[], size_t maxChanges,
                           bool &external);
        const_iterator addDocChangeNotifier(slice docID, DocChangeNotifier* NONNULL);
        void removeDocChangeNotifier(const_iterator, DocChangeNotifier* NONNULL);
        void removeObsoleteEntries();

    private:
        friend class CollectionChangeNotifier;
        friend class DocChangeNotifier;
        friend class SequenceTrackerTest;

        void _documentChanged(const alloc_slice &docID,
                              const alloc_slice &revID,
                              sequence_t sequence,
                              uint64_t bodySize,
                              RevisionFlags flags);
        const_iterator _since(sequence_t s) const;
        slice _docIDAt(sequence_t) const; // for tests only

        SequenceTracker(const SequenceTracker&) =delete;
        SequenceTracker& operator=(const SequenceTracker&) =delete;

        alloc_slice const                       _name;
        std::list<Entry>                        _changes;
        std::list<Entry>                        _idle;
        std::unordered_map<slice, iterator>     _byDocID;
        sequence_t                              _lastSequence {0};
        size_t                                  _numPlaceholders {0};
        size_t                                  _numDocObservers {0};
        unique_ptr<CollectionChangeNotifier>    _transaction;
        sequence_t                              _preTransactionLastSequence;
    };


    /** Tracks changes to a single document and calls a client callback. */
    class DocChangeNotifier {
    public:
        using Callback = std::function<void(DocChangeNotifier&, slice docID, sequence_t)>;

        DocChangeNotifier(SequenceTracker &t, slice docID, Callback cb);
        ~DocChangeNotifier();

        SequenceTracker &tracker;
        Callback const callback;

        slice docID() const;
        sequence_t sequence() const;

    protected:
        void notify(const SequenceTracker::Entry* entry) noexcept;

    private:
        DocChangeNotifier(const DocChangeNotifier&) =delete;
        DocChangeNotifier& operator=(const DocChangeNotifier&) =delete;

        friend class SequenceTracker;
        SequenceTracker::const_iterator const _docEntry;
    };


    /** Tracks changes to a collection and calls a client callback. */
    class CollectionChangeNotifier : public Logging {
    public:
        /** A callback that will be invoked _once_ when new changes arrive. After that, calling
            \ref readChanges will reset the state so the callback can be called again. */
        using Callback = std::function<void(CollectionChangeNotifier&)>;

        CollectionChangeNotifier(SequenceTracker&, Callback, sequence_t afterSeq =sequence_t::Max);

        ~CollectionChangeNotifier();

        SequenceTracker &tracker;
        Callback const callback;

        /** Returns true if there are new changes, i.e. if `readChanges` would return nonzero. */
        bool hasChanges() const {
            return tracker.hasChangesAfterPlaceholder(_placeholder);
        }

        /** Returns changes that have occurred since the last call to `readChanges` (or since
            construction.) A single call will return only internal or only external changes,
            setting the `external` flag to indicate which.
            To get all the changes, you need to keep calling this method until it returns 0.
            Only after that will the notifier reset so the callback can be called again. */
        size_t readChanges(SequenceTracker::Change changes[], size_t maxChanges, bool &external);

    protected:
        void notify() noexcept;

    private:
        CollectionChangeNotifier(const CollectionChangeNotifier&) =delete;
        CollectionChangeNotifier& operator=(const CollectionChangeNotifier&) =delete;

        friend class SequenceTracker;

        SequenceTracker::const_iterator const _placeholder;
    };

}
