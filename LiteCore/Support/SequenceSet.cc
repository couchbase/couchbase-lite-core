//
// SequenceSet.cc
//
// Copyright 2024-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "SequenceSet.hh"
#include "fleece/Fleece.hh"
#include <algorithm>
#include "betterassert.hh"

namespace litecore {
    using namespace fleece;

    [[nodiscard]] bool SequenceSet::contains(sequence begin, sequence end) const {
        assert(begin < end);
        auto i = _sequences.upper_bound(begin);  // first range with start > begin
        if ( i == _sequences.begin() ) return false;
        i = prev(i);  // now i is last range with start <= begin
        return end <= i->second;
    }

    // Implementation of add(s); returns an iterator pointing to the range containing `s`
    SequenceSet::Map::iterator SequenceSet::_add(sequence s) {
        // Possibilities:
        // * s is already contained within a range
        // * s is just before a range, so prepend it
        // * s is just after a range, so append it
        // * s fills a crack between two ranges (i.e. both of the above), so merge them
        // * s creates a new range of length 1

        auto upper = _sequences.upper_bound(s);  // first range with start > s
        if ( upper != _sequences.end() && s == upper->first - 1 ) {
            // s is just before upper; extend it or merge:
            if ( upper != _sequences.begin() ) {
                auto lower = prev(upper);
                if ( lower->second == s ) {
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

        if ( upper != _sequences.begin() ) {
            auto lower = prev(upper);
            if ( s < lower->second ) {
                // * Already contained
                return lower;
            } else if ( s == lower->second ) {
                // * Append s to lower:
                ++lower->second;
                return lower;
            }
        }

        // * Insert a singleton
        return _sequences.emplace_hint(upper, s, s + 1);
    }

    void SequenceSet::add(sequence s0, sequence s1) {
        assert(s1 >= s0);
        if ( s1 > s0 ) {
            auto lower = _add(s0);
            if ( s1 > s0 + 1 ) {
                auto upper = _add(s1 - 1);
                if ( upper != lower ) {
                    // Merge lower and upper, discarding any ranges in between:
                    lower->second = upper->second;
                    _sequences.erase(next(lower), next(upper));
                }
            }
        }
    }

    /** Removes a sequence from the set. */
    bool SequenceSet::remove(sequence s) {
        // Possibilities:
        // * s is not contained within a range
        // * s is in a range of length 1, so remove the range
        // * s is at the start of a range, so increment its start
        // * s is at the end of a range, so decrement its end
        // * s is in the middle of a range, so split the range

        auto i = _sequences.upper_bound(s);  // first range with start > s
        if ( i == _sequences.begin() ) return false;
        i = prev(i);

        if ( s >= i->second ) {
            // * not contained in a range
            return false;
        } else if ( s == i->first ) {
            if ( s == i->second - 1 ) {
                // * at the start & end: remove the range
                _sequences.erase(i);
            } else {
                // * at the start of a range
                _sequences.emplace_hint(next(i), s + 1, i->second);
                _sequences.erase(i);
            }
        } else if ( s == i->second - 1 ) {
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
    void SequenceSet::remove(sequence s0, sequence s1) {
        assert(s1 >= s0);
        if ( s1 > s0 ) {
            remove(s0);
            if ( s1 > s0 + 1 ) {
                remove(s1 - 1);
                if ( s1 > s0 + 2 ) {
                    // Remove any remaining ranges between s0 and s1:
                    auto begin = _sequences.upper_bound(s0);  // first range with start > s0
                    auto end   = begin;
                    while ( end != _sequences.end() && end->second <= s1 ) ++end;
                    _sequences.erase(begin, end);
                }
            }
        }
    }

    /** Creates an intersection with another sequence set to ensure that all missing
        sequences get accounted for */
    SequenceSet SequenceSet::intersection(const SequenceSet& s1, const SequenceSet& s2) {
        SequenceSet retVal;
        auto        i1 = s1.begin();
        auto        i2 = s2.begin();

        // Move through each of the sets and check if the current two are overlapping.
        // If so, add the overlapping range and then check below which set(s) to advance.
        while ( i1 != s1.end() && i2 != s2.end() ) {
            auto start = std::max(i1->first, i2->first);
            auto end   = std::min(i1->second, i2->second);
            if ( start < end ) { retVal.add(start, end); }

            // Need to evaluate this first before we advance anything
            // If the first end value is less than the second, advance only the first iterator
            // If the first end value is greater than the second, advance only the second iterator
            // If equal, advance both
            auto advanceFirst  = i1->second <= i2->second;
            auto advanceSecond = i1->second >= i2->second;

            if ( advanceFirst ) { ++i1; }

            if ( advanceSecond ) { ++i2; }
        }

        return retVal;
    }

    SequenceSet SequenceSet::difference(const SequenceSet& set1, const SequenceSet& set2) {
        SequenceSet result = set1;
        for ( auto [s1, s2] : set2 ) result.remove(s1, s2);
        return result;
    }

    SequenceSet SequenceSet::unionOf(const SequenceSet& set1, const SequenceSet& set2) {
        SequenceSet result = set1;
        for ( auto [s1, s2] : set2 ) result.add(s1, s2);
        return result;
    }

    void SequenceSet::encode_fleece(FLEncoder flEnc) const {
        Encoder enc(flEnc);
        enc.beginArray();
        for ( auto& range : _sequences ) {
            enc.writeUInt(uint64_t(range.first));
            enc.writeUInt(uint64_t(range.second - range.first));
        }
        enc.endArray();
        enc.detach();
    }

    bool SequenceSet::read_fleece(FLValue v) {
        Array ranges = Value(v).asArray();
        if ( !ranges || ranges.count() % 2 != 0 ) return false;
        for ( Array::iterator i(ranges); i; ++i ) {
            uint64_t first = i->asUnsigned();
            uint64_t end   = first + (++i)->asUnsigned();
            if ( end <= first ) return false;
            add(sequence(first), sequence(end));
        }
        return true;
    }

    fleece::alloc_slice SequenceSet::to_json() const {
        JSONEncoder enc;
        encode_fleece(enc);
        return enc.finish();
    }

    bool SequenceSet::read_json(fleece::slice json) { return read_fleece(Doc::fromJSON(json, nullptr).root()); }

}  // namespace litecore
