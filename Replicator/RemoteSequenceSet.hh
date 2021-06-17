//
// RemoteSequenceSet.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
#include "RemoteSequence.hh"
#include <map>
#include <utility>

namespace litecore { namespace repl {

    /** A set of opaque remote sequence IDs, representing server-side database sequences.
        This is used by the replicator to keep track of which revisions are being pulled. */
    class RemoteSequenceSet {
    public:
        RemoteSequenceSet()                     =default;

        /** Empties the set. */
        void clear(RemoteSequence since) {
            _sequences.clear();
            _nextOrder = 0;
            _lastAdded = since;
            _first = _sequences.end();
        }

        bool empty() const {
            return _sequences.empty();
        }

        size_t size() const {
            return _sequences.size();
        }

        /** Returns the sequence before the earliest one still in the set. */
        RemoteSequence since() {
            return (_first != _sequences.end()) ? _first->second.prevSequence : _lastAdded;
        }

        /** Adds a sequence to the set. */
        void add(RemoteSequence s, uint64_t bodySize) {
            bool wasEmpty = empty();
            auto p = _sequences.insert({s, {_nextOrder++, _lastAdded, bodySize}});
            _lastAdded = std::move(s);
            if (wasEmpty)
                _first = p.first;
        }

        /** Removes the sequence if it's in the set. Returns true if it was the earliest. */
        void remove(const RemoteSequence &s, bool &wasEarliest, uint64_t &outBodySize) {
            auto i = _sequences.find(s);
            if (i == _sequences.end()) {
                outBodySize = 0;
                wasEarliest = false;
                return;
            }
            outBodySize = i->second.bodySize;
            wasEarliest = (i == _first);
            if (wasEarliest) {
                size_t minOrder = i->second.order;
                _sequences.erase(i);
                findFirst(minOrder + 1);
            } else {
                _sequences.erase(i);
            }
        }

        uint64_t bodySizeOfSequence(const RemoteSequence &s) {
            auto i = _sequences.find(s);
            return (i == _sequences.end()) ? 0 : i->second.bodySize;
        }

    private:

        // Updates _first to point to the earliest entry in _sequences
        void findFirst(size_t minOrderInSet) {
            sequenceMap::iterator first = _sequences.end();
            size_t minOrderSoFar = SIZE_MAX;
            // OPT: Slow linear scan. Keep a secondary collection sorted by order?
            for (auto i = _sequences.begin(); i != _sequences.end(); ++i) {
                if (i->second.order < minOrderSoFar) {
                    minOrderSoFar = i->second.order;
                    first = i;
                    if (minOrderSoFar == minOrderInSet)
                        break;  // we know we've found the minimum
                }
            }
            _first = first;
        }

        struct value {
            size_t order;                   // Chronological order in which this sequence was added
            RemoteSequence prevSequence;    // The previously-added sequence
            uint64_t bodySize;              // Approx doc size, for client's use
        };
        using sequenceMap = std::map<RemoteSequence, value>;

        sequenceMap _sequences;             // Maps sequence to {order, previous seq}
        size_t _nextOrder {0};              // Order to assign to the next insertion
        RemoteSequence _lastAdded;          // The last sequence added
        sequenceMap::iterator _first {};    // Points to the earliest sequence still in _sequences
    };

} }
