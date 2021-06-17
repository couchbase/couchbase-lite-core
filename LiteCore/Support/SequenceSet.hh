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
#include <map>
#include <string>
#include <utility>
#include "betterassert.hh"

namespace litecore {

    /** A set of positive integers, generally representing database sequences.
        This is used by the replicator to keep track of which revisions are being pushed.

        \note The implementation is optimized for consecutive ranges of sequences: it stores
        ranges using a std::map that maps the first sequence in a range to the end of the range. */
    class SequenceSet {
    public:
        using sequence = uint64_t;
        using Map = std::map<sequence, sequence>;

        SequenceSet() =default;

        /** Empties the set. */
        void clear()                            {_sequences.clear();}

        /** Is the set empty? (This is faster than `size() == 0`.) */
        bool empty() const                      {return _sequences.empty();}

        /** The number of sequences in the set. */
        size_t size() const {
            size_t total = 0;
            for (auto &range : _sequences)
                total += range.second - range.first;
            return total;
        }

        /** The number of ranges of consecutive sequences in the set. */
        size_t rangesCount() const              {return _sequences.size();}

        /** Returns the lowest sequence in the set. If the set is empty, returns 0. */
        sequence first() const                  {return empty() ? 0 : _sequences.begin()->first;}

        /** Returns the highest sequence in the set. If the set is empty, returns 0. */
        sequence last() const                   {return empty() ? 0 : prev(_sequences.end())->second - 1;}

        /** Is the sequence in the set? */
        bool contains(sequence s) const {
            auto i = _sequences.upper_bound(s); // first range with start > s
            if (i == _sequences.begin())
                return false;
            i = prev(i);
            return s < i->second;
        }

        bool operator== (const SequenceSet &other) const  {return _sequences == other._sequences;}
        bool operator!= (const SequenceSet &other) const  {return _sequences != other._sequences;}

        /** Adds a sequence. */
        void add(sequence s) {
            (void)_add(s);
        }

        /** Adds all sequences in the range [s0...s1), _not including s1_ */
        void add(sequence s0, sequence s1) {
            assert (s1 >= s0);
            if (s1 > s0) {
                auto lower = _add(s0);
                if (s1 > s0 + 1) {
                    auto upper = _add(s1 - 1);
                    if (upper != lower) {
                        // Merge lower and upper, discarding any ranges in between:
                        lower->second = upper->second;
                        _sequences.erase(next(lower), next(upper));
                    }
                }
            }
        }

        /** Removes a sequence from the set. */
        bool remove(sequence s) {
            // Possibilities:
            // * s is not contained within a range
            // * s is in a range of length 1, so remove the range
            // * s is at the start of a range, so increment its start
            // * s is at the end of a range, so decrement its end
            // * s is in the middle of a range, so split the range

            auto i = _sequences.upper_bound(s); // first range with start > s
            if (i == _sequences.begin())
                return false;
            i = prev(i);
            
            if (s >= i->second) {
                // * not contained in a range
                return false;
            } else if (s == i->first) {
                if (s == i->second - 1) {
                    // * at the start & end: remove the range
                    _sequences.erase(i);
                } else {
                    // * at the start of a range
                    _sequences.emplace_hint(next(i), s + 1, i->second);
                    _sequences.erase(i);
                }
            } else if (s == i->second - 1) {
                // * at the end of a range
                i->second = s;
            } else {
                // * split the range:
                _sequences.emplace_hint(next(i), s + 1, i->second);
                i->second = s;
            }
            return true;
        }


        /** Removes all sequences in the range [s0...s1), _not including s1_ */
        void remove(sequence s0, sequence s1) {
            assert (s1 >= s0);
            if (s1 > s0) {
                remove(s0);
                if (s1 > s0 + 1) {
                    remove(s1 - 1);
                    if (s1 > s0 + 2) {
                        // Remove any remaining ranges between s0 and s1:
                        auto begin = _sequences.upper_bound(s0); // first range with start > s0
                        auto end = begin;
                        while (end != _sequences.end() && end->second <= s1)
                            ++end;
                        _sequences.erase(begin, end);
                    }
                }
            }
        }


        /** Iteration is over pair<sequence,sequence> values, where the first sequence is the
            start of a consecutive range, and the second sequence is the end of the range
            (one past the last sequence in the range.) */
        using const_iterator = Map::const_iterator;
        const_iterator begin() const                  {return _sequences.begin();}
        const_iterator end() const                    {return _sequences.end();}

        /** Returns a human-readable description, like "{1, 4, 7-9}". */
        std::string to_string() const;

    private:
        // Implementation of add; returns an iterator pointing to the range containing `s`
        Map::iterator _add(sequence s) {
            // Possibilities:
            // * s is already contained within a range
            // * s is just before a range, so prepend it
            // * s is just after a range, so append it
            // * s fills a crack between two ranges (i.e. both of the above), so merge them
            // * s creates a new range of length 1

            auto upper = _sequences.upper_bound(s); // first range with start > s
            if (upper != _sequences.end() && s == upper->first - 1) {
                // s is just before upper; extend it or merge:
                if (upper != _sequences.begin()) {
                    auto lower = prev(upper);
                    if (lower->second == s) {
                        // * Merge upper and lower
                        lower->second = upper->second;
                        _sequences.erase(upper);
                        return lower;
                    }
                }
                // * Prepend s to upper:
                auto newUpper = _sequences.emplace_hint(upper, s, upper->second);
                _sequences.erase(upper);
                return newUpper;
            }

            if (upper != _sequences.begin()) {
                auto lower = prev(upper);
                if (s < lower->second) {
                    // * Already contained
                    return lower;
                } else if (s == lower->second) {
                    // * Append s to lower:
                    ++lower->second;
                    return lower;
                }
            }

            // * Insert a singleton
            return _sequences.emplace_hint(upper, s, s + 1);
        }

        Map _sequences;    // Maps start of range --> end of range (exclusive)
    };

}
