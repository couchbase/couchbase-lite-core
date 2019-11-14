//
// SequenceSet.hh
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
#include "function_ref.hh"
#include <set>
#include <utility>

namespace litecore {

    /** A set of positive integers, generally representing database sequences.
        This is used by the replicator to keep track of which revisions are being pushed. */
    class SequenceSet {
    public:
        typedef uint64_t sequence;

        SequenceSet() { }

        /** Empties the set.
            The optional `max` parameter sets the initial value of the `maxEver` property. */
        void clear(sequence max =0)             {_sequences.clear(); _max = max;}

        bool empty() const                      {return _sequences.empty();}
        size_t size() const                     {return _sequences.size();}

        /** Returns the lowest sequence in the set. If the set is empty, returns 0. */
        sequence first() const                  {return empty() ? 0 : *_sequences.begin();}

        /** Returns the highest sequence in the set. If the set is empty, returns 0. */
        sequence last() const                   {return empty() ? 0 : *prev(_sequences.end());}

        /** The largest sequence ever stored in the set. (The clear() function resets this.) */
        sequence maxEver() const                {return _max;}

        /** The largest sequence to have no pending sequences before it. The "checkpoint". */
        sequence lastComplete() const           {return empty() ? _max : *_sequences.begin() - 1;}

        bool hasRemoved(sequence s) const       {return s <= _max && _sequences.find(s) == _sequences.end();}

        void add(sequence s)                    {_sequences.insert(s); _max = std::max(_max, s);}

        void add(sequence s0, sequence s1) {
            for (sequence s = s0; s <= s1; ++s)
                _sequences.insert(s);
            _max = std::max(_max, s1);
        }

        void remove(sequence s)                 {_sequences.erase(s);}

        /** Marks a sequence as seen but not in the set; equivalent to add() then remove(). */
        void seen(sequence s)                   {_max = std::max(_max, s);}

        using iterator = std::set<sequence>::const_iterator;
        iterator begin() const                  {return _sequences.begin();}
        iterator end() const                    {return _sequences.end();}

        /** Iterates over consecutive ranges of sequences, invoking the callback for each.
            The parameters to the callback are the first and last sequence in a range. */
        void ranges(fleece::function_ref<void(sequence, sequence)> callback) const {
            sequence first = UINT64_MAX, prev = 0;
            for (sequence s : _sequences) {
                if (s != prev+1) {
                    if (first <= prev)
                        callback(first, prev);
                    first = s;
                }
                prev = s;
            }
            if (first <= prev)
                callback(first, prev);
        }

    private:
        std::set<sequence> _sequences;
        sequence _max {0};
    };

}
