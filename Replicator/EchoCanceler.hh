//
// EchoCanceler.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "Logging.hh"
#include "ReplicatorTypes.hh"
#include <mutex>
#include <unordered_map>
#include <vector>

C4_ASSUME_NONNULL_BEGIN

namespace litecore::repl {

    /** A set of {docID, revID} pairs that's used to avoid echoing a peer's own revisions back to it.
        (There's actually one of these sets per collection, but only for collections that have
        bidirectional continuous replication.)
        - The Puller's Inserter adds revisions before it inserts them in the database.
        - The Pusher's ChangesFeed checks new local revisions and ignores ones that are in the set. */
    class EchoCanceler {
      public:
        /// Enables tracking revisions in the collection with this index.
        void trackCollection(CollectionIndex ci) {
            std::unique_lock lock(_mutex);
            if ( ci >= _collections.size() ) _collections.resize(ci + 1);
            if ( !_collections[ci] ) _collections[ci] = std::make_unique<RevMap>();
        }

        /// Adds a revision to a collection's set (if that collection is tracking.)
        /// @note This is called by the Inserter after it saves an incoming revision.
        void addRev(CollectionIndex ci, alloc_slice docID, alloc_slice revID) {
            std::unique_lock lock(_mutex);
            if ( RevMap* revMap = mapForCollection(ci) ) {
                if ( revMap->size() >= kMaxRevs ) removeOldest(revMap);
                revMap->emplace(std::move(docID), pair{std::move(revID), c4_now()});
            }
        }

        /// Returns true if a revision has been added to a collection's set.
        /// Removes the revision as a side effect, since it won't be needed again.
        /// @note This is called by the ReplicatorChangesFeed when it observes new revisions.
        bool revIsEchoed(CollectionIndex ci, alloc_slice const& docID, slice revID) {
            std::unique_lock lock(_mutex);
            if ( RevMap* revMap = mapForCollection(ci) ) {
                for ( auto [i, end] = revMap->equal_range(docID); i != end; ++i ) {
                    if ( i->second.first == revID ) {
                        revMap->erase(i);
                        return true;
                    }
                }
            }
            return false;
        }

      private:
        // Normally revisions won't stay in the map for long, but if the ChangesFeed has a filter
        // it won't see & remove all revs added by the Inserter, so the EchoCanceler handles
        // overflow by 'forgetting' the earliest-added revs.
        static constexpr size_t kMaxRevs = 250;

        using RevMap = std::unordered_multimap<alloc_slice, std::pair<alloc_slice, C4Timestamp>>;

        RevMap* C4NULLABLE mapForCollection(CollectionIndex ci) const {
            return (ci < _collections.size()) ? _collections[ci].get() : nullptr;
        }

        void removeOldest(RevMap* revMap) {
            auto imin = std::ranges::min_element(*revMap,
                                                 [](auto& a, auto& b) { return a.second.second < b.second.second; });
            LogDebug(SyncLog, "EchoSet: pruning rev %.*s %.*s", FMTSLICE(imin->first), FMTSLICE(imin->second.first));
            revMap->erase(imin);
        }

        std::mutex                           _mutex;
        std::vector<std::unique_ptr<RevMap>> _collections;
    };

}  // namespace litecore::repl

C4_ASSUME_NONNULL_END
