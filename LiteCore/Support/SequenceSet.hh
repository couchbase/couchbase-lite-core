//
// SequenceSet.hh
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
#include "c4Base.h"
#include "fleece/FLBase.h"
#include "fleece/slice.hh"
#include <map>
#include <string>
#include <utility>

namespace litecore {

    /** A set of positive integers, generally representing database sequences.
        This is used by the replicator to keep track of which revisions are being pushed.

        \note The implementation is optimized for consecutive ranges of sequences: it stores
        ranges using a std::map that maps the first sequence in a range to the end of the range. */
    class SequenceSet {
      public:
        using sequence = C4SequenceNumber;
        using Map      = std::map<sequence, sequence>;

        SequenceSet() = default;

        /** Empties the set. */
        void clear() { _sequences.clear(); }

        /** Is the set empty? (This is faster than `size() == 0`.) */
        [[nodiscard]] bool empty() const { return _sequences.empty(); }

        /** The number of sequences in the set. */
        [[nodiscard]] size_t size() const {
            size_t total = 0;
            for ( auto& range : _sequences ) total += range.second - range.first;
            return total;
        }

        /** The number of ranges of consecutive sequences in the set. */
        [[nodiscard]] size_t rangesCount() const { return _sequences.size(); }

        /** Returns the lowest sequence in the set. If the set is empty, returns 0. */
        [[nodiscard]] sequence first() const { return empty() ? sequence(0) : _sequences.begin()->first; }

        /** Returns the highest sequence in the set. If the set is empty, returns 0. */
        [[nodiscard]] sequence last() const { return empty() ? sequence(0) : prev(_sequences.end())->second - 1; }

        /** Is the sequence in the set? */
        [[nodiscard]] bool contains(sequence s) const { return contains(s, s + 1); }

        /** Is the entire range of sequences [begin...end) in the set? */
        [[nodiscard]] bool contains(sequence begin, sequence end) const;

        bool operator==(const SequenceSet& other) const { return _sequences == other._sequences; }

        bool operator!=(const SequenceSet& other) const { return _sequences != other._sequences; }

        //======== MODIFICATION:

        /** Adds a sequence. */
        void add(sequence s) { (void)_add(s); }

        /** Adds all sequences in the range [s0...s1), _not including s1_ */
        void add(sequence s0, sequence s1);

        /** Removes a sequence from the set. */
        bool remove(sequence s);

        /** Removes all sequences in the range [s0...s1), _not including s1_ */
        void remove(sequence s0, sequence s1);

        //======== SET OPERATIONS:

        /** Returns the union of s1 and s2, i.e. all sequences that are in either.  */
        [[nodiscard]] static SequenceSet unionOf(const SequenceSet& s1, const SequenceSet& s2);

        /** Returns the intersection of s1 and s2, i.e. all sequences that are in both.  */
        [[nodiscard]] static SequenceSet intersection(const SequenceSet& s1, const SequenceSet& s2);

        /** Subtracts s2 from s1, i.e. returns the set of sequences in s1 but not s2. */
        [[nodiscard]] static SequenceSet difference(const SequenceSet& s1, const SequenceSet& s2);

        friend SequenceSet operator|(const SequenceSet& s1, const SequenceSet& s2) { return unionOf(s1, s2); }

        friend SequenceSet operator&(const SequenceSet& s1, const SequenceSet& s2) { return intersection(s1, s2); }

        friend SequenceSet operator-(const SequenceSet& s1, const SequenceSet& s2) { return difference(s1, s2); }

        //======== ITERATION:

        /** Iteration is over pair<sequence,sequence> values, where the first sequence is the
            start of a consecutive range, and the second sequence is the end of the range
            (one past the last sequence in the range.) */
        using const_iterator = Map::const_iterator;

        [[nodiscard]] const_iterator begin() const { return _sequences.begin(); }

        [[nodiscard]] const_iterator end() const { return _sequences.end(); }

        //======== I/O:

        /** Encodes as JSON, which can be reconstituted with `read_json`. */
        [[nodiscard]] fleece::alloc_slice to_json() const;

        /** Reads the JSON encoding created by `to_json`; returns false if not parseable.
            The sequences read from JSON will be added to any existing ones in this instance. */
        [[nodiscard]] bool read_json(fleece::slice json);

        /** Writes a description to a Fleece encoder. */
        void encode_fleece(FLEncoder) const;

        /** Reads the encoded form written by `encode_fleece`; returns false if not parseable.
            The sequences read will be added to any existing ones in this instance. */
        [[nodiscard]] bool read_fleece(FLValue);

        /** Returns a human-readable description, like "{1, 4, 7-9}". */
        [[nodiscard]] std::string to_string() const;

      private:
        Map::iterator _add(sequence s);

        Map _sequences;  // Maps start of range --> end of range (exclusive)
    };

}  // namespace litecore
