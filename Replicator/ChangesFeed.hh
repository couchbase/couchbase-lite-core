//
// ChangesFeed.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "Worker.hh"
#include <memory>
#include <string>
#include <unordered_set>

struct C4DocumentInfo;

namespace litecore::repl {
    class Pusher;
    class Checkpointer;
    class RevToSend;

    using DocIDSet = std::shared_ptr<std::unordered_set<std::string>>;

    /** Queries the database to find revisions for the Pusher to send. */
    class ChangesFeed : public Logging {
    public:
        ChangesFeed(Pusher&, Options&, std::shared_ptr<DBAccess>, Checkpointer&);

        void skipDeletedDocs(bool skip)             {_skipDeleted = skip;}
        
        void getForeignAncestors(bool get)          {_getForeignAncestors = get;}

        /** Filters to the docIDs in the given Fleece array.
            If a filter already exists, the two will be intersected. */
        void filterByDocIDs(fleece::Array docIDs);

        void setLastSequence(C4SequenceNumber s)    {_maxPushedSequence = s;}

        void getMoreChanges();

        void dbChanged();

        bool shouldPushRev(RevToSend* NONNULL);

    private:
        void getObservedChanges();
        bool getRemoteRevID(RevToSend *rev NONNULL, C4Document *doc NONNULL);
        Retained<RevToSend> revToSend(C4DocumentInfo&, C4DocEnumerator*, C4Database* NONNULL);
        bool shouldPushRev(RevToSend*, C4DocEnumerator*, C4Database* NONNULL);

        Pusher& _pusher;
        Options &_options;
        std::shared_ptr<DBAccess> _db;
        Checkpointer& _checkpointer;
        DocIDSet _docIDs;                                   // Doc IDs to filter to, or null
        c4::ref<C4DatabaseObserver> _changeObserver;        // Used in continuous push mode
        C4SequenceNumber _maxPushedSequence {0};            // Latest seq that's been pushed
        bool _getForeignAncestors {true};                   // True in propose-changes mode
        bool _skipDeleted {false};                          // True if skipping tombstones
        bool _passive;                                      // True if replicator is passive
        bool _waitingForObservedChanges {false};            // True if expecting change notification
    };

}
