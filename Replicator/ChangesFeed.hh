//
// ChangesFeed.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "ReplicatorTypes.hh"
#include "Logging.hh"
#include <memory>
#include <string>
#include <unordered_set>

struct C4DocumentInfo;

namespace litecore::repl {
    class DBAccess;
    class Options;
    class Pusher;
    class Checkpointer;

    using DocIDSet = std::shared_ptr<std::unordered_set<std::string>>;

    /** Queries the database to find revisions for the Pusher to send. */
    class ChangesFeed : public Logging {
    public:
        class Delegate {
        public:
            virtual ~Delegate() { };
            /** Callback when new changes are available. Only called in continuous mode, after catching
                up, and then only after `getMoreChanges` has returned less than the limit. It will only
                be called once until the next call to `getMoreChanges`.
                \warning This is called on an arbitrary thread! */
            virtual void dbHasNewChanges() =0;
            /** Called if `getMoreChanges` encounters an error reading a document while deciding
                whether to include it.*/
            virtual void failedToGetChange(ReplicatedRev *rev, C4Error error, bool transient) =0;
        };

        ChangesFeed(Delegate&, Options&, std::shared_ptr<DBAccess>, Checkpointer&);

        // Setup:
        void setContinuous(bool continuous)         {_continuous = continuous;}
        void setLastSequence(C4SequenceNumber s)    {_maxSequence = s;}
        void setSkipDeletedDocs(bool skip)          {_skipDeleted = skip;}
        void setFindForeignAncestors(bool use)      {_getForeignAncestors = use;}
        void setCheckpointValid(bool valid)         {_isCheckpointValid = valid;}

        /** Filters to the docIDs in the given Fleece array.
            If a filter already exists, the two will be intersected. */
        void filterByDocIDs(fleece::Array docIDs);

        struct Changes {
            std::shared_ptr<RevToSendList> revs;    // Ordered list of new revisions
            C4SequenceNumber lastSequence;          // The last sequence that was checked
            C4Error err;                            // On failure, error goes here
        };

        /** Returns up to `limit` more changes.
            If exactly `limit` are returned, there may be more, so the client should call again. */
        Changes getMoreChanges(unsigned limit) MUST_USE_RESULT;

        /** True after all historical changes have been returned from `getMoreChanges`. */
        bool caughtUp() const                       {return _caughtUp;}

        /** Returns true if the given rev matches the push filters. */
        bool shouldPushRev(RevToSend* NONNULL) const MUST_USE_RESULT;

    protected:
        std::string loggingClassName() const override;

    private:
        Changes getHistoricalChanges(unsigned limit);
        Changes getObservedChanges(unsigned limit);
        void _dbChanged();
        bool getRemoteRevID(RevToSend *rev NONNULL, C4Document *doc NONNULL) const;
        Retained<RevToSend> revToSend(C4DocumentInfo&, C4DocEnumerator*, C4Database* NONNULL);
        bool shouldPushRev(RevToSend*, C4DocEnumerator*, C4Database* NONNULL) const;

        Delegate& _delegate;
        Options &_options;
        std::shared_ptr<DBAccess> _db;
        Checkpointer& _checkpointer;
        DocIDSet _docIDs;                                   // Doc IDs to filter to, or null
        c4::ref<C4DatabaseObserver> _changeObserver;        // Used in continuous push mode
        C4SequenceNumber _maxSequence {0};                  // Latest sequence I've read
        bool _continuous;                                   // Continuous mode
        bool _passive;                                      // True if replicator is passive
        bool _skipDeleted {false};                          // True if skipping tombstones
        bool _getForeignAncestors {true};                   // True in propose-changes mode
        bool _isCheckpointValid {true};
        bool _caughtUp {false};                             // Delivered all historical changes
        std::atomic<bool> _notifyOnChanges {false};         // True if expecting change notification
    };

}
