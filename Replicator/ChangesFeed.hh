//
// ChangesFeed.hh
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "ReplicatorTypes.hh"
#include "Logging.hh"
#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>

struct C4DocumentInfo;

namespace fleece {
    class Array;
}

namespace litecore::repl {
    class DBAccess;
    class Options;
    class Checkpointer;

    using DocIDSet = std::shared_ptr<std::unordered_set<std::string>>;

    /** Queries the database to find revisions for the Pusher to send. */
    class ChangesFeed : public Logging {
      public:
        class Delegate {
          public:
            virtual ~Delegate() = default;
            ;
            /** Callback when new changes are available. Only called in continuous mode, after catching
                up, and then only after `getMoreChanges` has returned less than the limit. It will only
                be called once until the next call to `getMoreChanges`.
                \warning This is called on an arbitrary thread! */
            virtual void dbHasNewChanges() = 0;
            /** Called if `getMoreChanges` encounters an error reading a document while deciding
                whether to include it.*/
            virtual void failedToGetChange(ReplicatedRev* rev, C4Error error, bool transient) = 0;
        };

        ChangesFeed(Delegate&, const Options* NONNULL, DBAccess& db, Checkpointer*);
        ~ChangesFeed() override;

        // Setup:
        void setContinuous(bool continuous) { _continuous = continuous; }

        void setLastSequence(C4SequenceNumber s) { _maxSequence = s; }

        void setSkipDeletedDocs(bool skip) { _skipDeleted = skip; }

        void setCheckpointValid(bool valid) { _isCheckpointValid = valid; }

        /** Filters to the docIDs in the given Fleece array.
            If a filter already exists, the two will be intersected. */
        void filterByDocIDs(fleece::Array docIDs);

        struct Changes {
            RevToSendList    revs;           // Ordered list of new revisions
            C4SequenceNumber firstSequence;  // The first sequence that was checked
            C4SequenceNumber lastSequence;   // The last sequence that was checked
            C4Error          err;            // On failure, error goes here
            bool             askAgain;       // Should client call getMoreChanges again?
        };

        /** Returns up to `limit` more changes.
            If exactly `limit` are returned, there may be more, so the client should call again. */
        [[nodiscard]] virtual Changes getMoreChanges(unsigned limit);

        C4SequenceNumber lastSequence() const { return _maxSequence; }

        /** True after all historical changes have been returned from `getMoreChanges`. */
        bool caughtUp() const { return _caughtUp; }

        /** Returns true if the given rev matches the push filters. */
        [[nodiscard]] virtual bool shouldPushRev(RevToSend* NONNULL) const;

      protected:
        std::string loggingClassName() const override { return "ChangesFeed"; }

        virtual bool getRemoteRevID(RevToSend* rev NONNULL, C4Document* doc NONNULL) const;

      private:
        void                getHistoricalChanges(Changes&, unsigned limit);
        void                getObservedChanges(Changes&, unsigned limit);
        void                _dbChanged();
        Retained<RevToSend> makeRevToSend(C4DocumentInfo&, C4DocEnumerator*);
        bool                shouldPushRev(RevToSend*, C4DocEnumerator*) const;

      protected:
        Delegate&              _delegate;
        RetainedConst<Options> _options;
        DBAccess&              _db;
        C4CollectionSpec const _collectionSpec;
        CollectionIndex const  _collectionIndex;
        bool                   _getForeignAncestors{false};  // True in propose-changes mode
      private:
        Checkpointer*                       _checkpointer;
        DocIDSet                            _docIDs;              // Doc IDs to filter to, or null
        std::unique_ptr<C4DatabaseObserver> _changeObserver;      // Used in continuous push mode
        C4SequenceNumber                    _maxSequence{0};      // Latest sequence I've read
        bool                                _continuous;          // Continuous mode
        bool                                _skipDeleted{false};  // True if skipping tombstones
        bool                                _isCheckpointValid{true};
        bool                                _caughtUp{false};         // Delivered all historical changes
        std::atomic<bool>                   _notifyOnChanges{false};  // True if expecting change notification
    };

    class ReplicatorChangesFeed final : public ChangesFeed {
      public:
        ReplicatorChangesFeed(Delegate& delegate, const Options* options, DBAccess& db, Checkpointer* cp);

        void setFindForeignAncestors(bool use) { _getForeignAncestors = use; }

        [[nodiscard]] Changes getMoreChanges(unsigned limit) override;

      protected:
        bool getRemoteRevID(RevToSend* rev NONNULL, C4Document* doc NONNULL) const override;

      private:
        const bool _usingVersionVectors;
    };
}  // namespace litecore::repl
