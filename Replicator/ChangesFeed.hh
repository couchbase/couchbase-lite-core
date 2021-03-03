//
// ChangesFeed.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
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
    struct Options;
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

        ChangesFeed(Delegate&, Options&, DBAccess &db, Checkpointer*);

        // Setup:
        void setContinuous(bool continuous)         {_continuous = continuous;}
        void setLastSequence(C4SequenceNumber s)    {_maxSequence = s;}
        void setEchoLocalChanges(bool echo)         {_echoLocalChanges = echo;}
        void setSkipDeletedDocs(bool skip)          {_skipDeleted = skip;}
        void setCheckpointValid(bool valid)         {_isCheckpointValid = valid;}

        /** Filters to the docIDs in the given Fleece array.
            If a filter already exists, the two will be intersected. */
        void filterByDocIDs(fleece::Array docIDs);

        struct Changes {
            RevToSendList revs;                     // Ordered list of new revisions
            C4SequenceNumber firstSequence;         // The first sequence that was checked
            C4SequenceNumber lastSequence;          // The last sequence that was checked
            C4Error err;                            // On failure, error goes here
            bool askAgain;                          // Should client call getMoreChanges again?
        };

        /** Returns up to `limit` more changes.
            If exactly `limit` are returned, there may be more, so the client should call again. */
        virtual Changes getMoreChanges(unsigned limit) MUST_USE_RESULT;

        C4SequenceNumber lastSequence() const       {return _maxSequence;}

        /** True after all historical changes have been returned from `getMoreChanges`. */
        bool caughtUp() const                       {return _caughtUp;}

        /** Returns true if the given rev matches the push filters. */
        virtual bool shouldPushRev(RevToSend* NONNULL) const MUST_USE_RESULT;

    protected:
        std::string loggingClassName() const override;
        virtual bool getRemoteRevID(RevToSend *rev NONNULL, C4Document *doc NONNULL) const;

    private:
        void getHistoricalChanges(Changes&, unsigned limit);
        void getObservedChanges(Changes&, unsigned limit);
        void _dbChanged();
        Retained<RevToSend> makeRevToSend(C4DocumentInfo&, C4DocEnumerator*, C4Database* NONNULL);
        bool shouldPushRev(RevToSend*, C4DocEnumerator*, C4Database* NONNULL) const;

    protected:
        Delegate& _delegate;
        Options &_options;
        DBAccess& _db;
        bool _getForeignAncestors {false};                  // True in propose-changes mode
    private:
        Checkpointer* _checkpointer;
        DocIDSet _docIDs;                                   // Doc IDs to filter to, or null
        c4::ref<C4DatabaseObserver> _changeObserver;        // Used in continuous push mode
        C4SequenceNumber _maxSequence {0};                  // Latest sequence I've read
        bool _continuous;                                   // Continuous mode
        bool _passive;                                      // True if replicator is passive
        bool _echoLocalChanges {false};                     // True if including changes made by _db
        bool _skipDeleted {false};                          // True if skipping tombstones
        bool _isCheckpointValid {true};
        bool _caughtUp {false};                             // Delivered all historical changes
        std::atomic<bool> _notifyOnChanges {false};         // True if expecting change notification
    };


    class ReplicatorChangesFeed : public ChangesFeed {
    public:
        ReplicatorChangesFeed(Delegate &delegate, Options &options, DBAccess &db, Checkpointer *cp);

        void setFindForeignAncestors(bool use)      {_getForeignAncestors = use;}

        virtual Changes getMoreChanges(unsigned limit) override MUST_USE_RESULT;

    protected:
        bool getRemoteRevID(RevToSend *rev NONNULL, C4Document *doc NONNULL) const override;
    private:
        const bool _usingVersionVectors;
    };
}
