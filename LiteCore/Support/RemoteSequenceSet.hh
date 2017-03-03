//
//  RemoteSequenceSet.hh
//  LiteCore
//
//  Created by Jens Alfke on 3/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include <assert.h>
#include <map>

namespace litecore { namespace repl {

    /** A set of opaque remote sequence IDs, representing server-side database sequences.
        This is used by the replicator to keep track of which revisions are being pulled. */
    class RemoteSequenceSet {
    public:
        typedef fleece::alloc_slice sequence;
        class reference;

        RemoteSequenceSet()                     { }

        /** Empties the set. */
        void clear(sequence since) {
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
        sequence since() {
            return (_first != _sequences.end()) ? _first->second.prevSequence : _lastAdded;
        }

        /** Adds a sequence to the set. */
        void add(sequence s) {
            bool wasEmpty = empty();
            auto p = _sequences.insert({s, {_nextOrder++, _lastAdded}});
            _lastAdded = s;
            if (wasEmpty)
                _first = p.first;
        }

        /** Removes the sequence if it's in the set. Returns true if it was the earliest. */
        bool remove(sequence s) {
            auto i = _sequences.find(s);
            if (i == _sequences.end()) {
                return false;
            } else if (i == _first) {
                size_t minOrder = i->second.order;
                _sequences.erase(i);
                findFirst(minOrder + 1);
                return true;
            } else {
                _sequences.erase(i);
                return false;
            }
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
            sequence prevSequence;          // The previously-added sequence
        };
        using sequenceMap = std::map<sequence, value>;

        sequenceMap _sequences;             // Maps sequence to {order, previous seq}
        size_t _nextOrder {0};              // Order to assign to the next insertion
        sequence _lastAdded;                // The last sequence added
        sequenceMap::iterator _first {};    // Points to the earliest sequence still in _sequences
    };

} }
