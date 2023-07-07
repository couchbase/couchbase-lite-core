//
// RemoteSequenceSet.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "RemoteSequence.hh"
#include <map>
#include <utility>

namespace litecore::repl {

    /** A set of opaque remote sequence IDs, representing server-side database sequences.
        This is used by the replicator to keep track of which revisions are being pulled. */
    class RemoteSequenceSet {
      public:
        RemoteSequenceSet() = default;

        /** Empties the set. */
        void clear(RemoteSequence since) {
            _sequences.clear();
            _nextOrder = 0;
            _lastAdded = std::move(since);
            _first     = _sequences.end();
        }

        [[nodiscard]] bool empty() const { return _sequences.empty(); }

        [[nodiscard]] size_t size() const { return _sequences.size(); }

        /** Returns the sequence before the earliest one still in the set. */
        RemoteSequence since() { return (_first != _sequences.end()) ? _first->second.prevSequence : _lastAdded; }

        /** Adds a sequence to the set. */
        void add(RemoteSequence s, uint64_t bodySize) {
            bool wasEmpty = empty();
            auto p        = _sequences.insert({s, {_nextOrder++, _lastAdded, bodySize}});
            _lastAdded    = std::move(s);
            if ( wasEmpty ) _first = p.first;
        }

        /** Removes the sequence if it's in the set. Returns true if it was the earliest. */
        void remove(const RemoteSequence& s, bool& wasEarliest, uint64_t& outBodySize) {
            auto i = _sequences.find(s);
            if ( i == _sequences.end() ) {
                outBodySize = 0;
                wasEarliest = false;
                return;
            }
            outBodySize = i->second.bodySize;
            wasEarliest = (i == _first);
            if ( wasEarliest ) {
                size_t minOrder = i->second.order;
                _sequences.erase(i);
                findFirst(minOrder + 1);
            } else {
                _sequences.erase(i);
            }
        }

        uint64_t bodySizeOfSequence(const RemoteSequence& s) {
            auto i = _sequences.find(s);
            return (i == _sequences.end()) ? 0 : i->second.bodySize;
        }

      private:
        // Updates _first to point to the earliest entry in _sequences
        void findFirst(size_t minOrderInSet) {
            auto   first         = _sequences.end();
            size_t minOrderSoFar = SIZE_MAX;
            // OPT: Slow linear scan. Keep a secondary collection sorted by order?
            for ( auto i = _sequences.begin(); i != _sequences.end(); ++i ) {
                if ( i->second.order < minOrderSoFar ) {
                    minOrderSoFar = i->second.order;
                    first         = i;
                    if ( minOrderSoFar == minOrderInSet ) break;  // we know we've found the minimum
                }
            }
            _first = first;
        }

        struct value {
            size_t         order;         // Chronological order in which this sequence was added
            RemoteSequence prevSequence;  // The previously-added sequence
            uint64_t       bodySize;      // Approx doc size, for client's use
        };

        using sequenceMap = std::map<RemoteSequence, value>;

        sequenceMap           _sequences;     // Maps sequence to {order, previous seq}
        size_t                _nextOrder{0};  // Order to assign to the next insertion
        RemoteSequence        _lastAdded;     // The last sequence added
        sequenceMap::iterator _first{};       // Points to the earliest sequence still in _sequences
    };

}  // namespace litecore::repl
