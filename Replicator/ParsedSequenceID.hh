//
// ParsedSequenceID.hh
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
#include "StringUtil.hh"
#include "slice_stream.hh"
#include <cstdint>
#include <string>

namespace litecore::repl {

    /**
     * Represents a parsed Sync Gateway compound sequence ID.
     *
     * Sync Gateway emits sequence IDs in three formats on the _changes feed:
     *
     *   - Simple:    "Seq"                  e.g. "42"
     *                A plain database sequence number.
     *
     *   - Backfill:  "TriggeredBy:Seq"      e.g. "100:35"
     *                A revision (seq 35) is being sent retroactively because an access-grant
     *                change at sequence 100 gave the user access to its channel.
     *
     *   - LowSeq:    "LowSeq:TriggeredBy:Seq"   e.g. "20:100:35"
     *                Same as backfill, but also records the lowest contiguous sequence (20)
     *                that the client has fully processed on the feed. TriggeredBy may be
     *                omitted (e.g. "20::35") when there is no access-grant trigger.
     *
     * For checkpoint comparison the "comparison value" of each format is:
     *   - Simple   → seq
     *   - Backfill → triggeredBy
     *   - LowSeq   → lowSeq
     *
     * The before() method implements the ordering rules that match Sync Gateway's
     * SequenceID.Before(), so that LiteCore and SG always agree on which checkpoint is older.
     */

    class ParsedSequenceID {
      public:
        ParsedSequenceID() = default;

        ParsedSequenceID(uint64_t seq, uint64_t triggeredBy, uint64_t lowSeq)
            : _seq(seq), _triggeredBy(triggeredBy), _lowSeq(lowSeq) {}

        [[nodiscard]] uint64_t seq() const { return _seq; }

        [[nodiscard]] uint64_t triggeredBy() const { return _triggeredBy; }

        [[nodiscard]] uint64_t lowSeq() const { return _lowSeq; }

        /// "Seq" — plain sequence, no trigger, no lowSeq.
        [[nodiscard]] bool isSimpleRevision() const { return _lowSeq == 0 && _triggeredBy == 0; }

        /// "TriggeredBy:Seq" — sent retroactively due to an access-grant, no lowSeq tracking.
        /// triggeredBy is the primary comparison value in before().
        [[nodiscard]] bool isTriggeredRevision() const { return _lowSeq == 0 && _triggeredBy > 0; }

        /// "LowSeq:TriggeredBy:Seq" or "LowSeq::Seq" — includes lowSeq feed-position tracking.
        /// lowSeq is the primary comparison value in before(). May or may not include a trigger.
        [[nodiscard]] bool isLowSeqRevision() const { return _lowSeq > 0; }

        /**
         * Parse a sequence string into a ParsedSequenceID.
         *
         * Accepted formats:
         *   "42"          → {seq=42, triggeredBy=0, lowSeq=0}
         *   "100:35"      → {seq=35, triggeredBy=100, lowSeq=0}
         *   "20:100:35"   → {seq=35, triggeredBy=100, lowSeq=20}
         *   "20::35"      → {seq=35, triggeredBy=0,   lowSeq=20}
         *
         * @param str  The string to parse.
         * @param out  Receives the parsed result on success.
         * @return true on success, false if the string is empty, malformed, or has
         *         an unexpected number of components.
         */
        static bool parse(const std::string& str, ParsedSequenceID& out) {
            if ( str.empty() || str.back() == ':' ) return false;  // litecore::split drops trailing empty tokens

            auto parts = litecore::split(str, ":");
            if ( parts.size() < 1 || parts.size() > 3 ) return false;

            // Parse a single component as uint64. Empty is only valid for middle part of "LowSeq::Seq".
            auto readUInt = [](std::string_view sv, uint64_t& val) -> bool {
                if ( sv.empty() ) return false;
                fleece::slice_istream stream(sv.data(), sv.size());
                val = stream.readDecimal();
                return stream.eof();
            };

            uint64_t v[3] = {};
            switch ( parts.size() ) {
                case 1:
                    if ( !readUInt(parts[0], v[0]) ) return false;
                    out = ParsedSequenceID(v[0], 0, 0);  // Seq
                    return true;
                case 2:
                    if ( !readUInt(parts[0], v[0]) || !readUInt(parts[1], v[1]) ) return false;
                    out = ParsedSequenceID(v[1], v[0], 0);  // TriggeredBy:Seq
                    return true;
                case 3:
                    if ( !readUInt(parts[0], v[0]) ) return false;
                    if ( !parts[1].empty() && !readUInt(parts[1], v[1]) ) return false;  // empty → 0 for "LowSeq::Seq"
                    if ( !readUInt(parts[2], v[2]) ) return false;
                    out = ParsedSequenceID(v[2], v[1], v[0]);  // LowSeq:TriggeredBy:Seq
                    return true;
                default:
                    return false;
            }
        }

        /**
         * Returns true if this sequence is strictly before \p seqID2.
         *
         * Implements the same ordering as Sync Gateway's SequenceID.Before().
         * Each format has a "comparison value" (CV):
         *   - Simple   → CV = seq
         *   - Backfill → CV = triggeredBy
         *   - LowSeq   → CV = lowSeq
         *
         * Cross-format rules (a.before(b)):
         *
         *   a \ b          | Simple       | Backfill        | LowSeq
         *   ───────────────|──────────────|─────────────────|────────────────
         *   Simple         | seq < seq    | seq <  trig     | seq <= low
         *   Backfill       | trig <= seq  | trig < trig     | trig <= low
         *                  |              | (tie: seq<seq)  |
         *   LowSeq         | low <  seq   | low <  trig     | low < low
         *                  |              |                 | (tie: recurse
         *                  |              |                 |  on inner pair)
         *
         */
        [[nodiscard]] bool before(const ParsedSequenceID& seqID2) const {
            const auto& a = *this;
            const auto& b = seqID2;

            if ( a.isLowSeqRevision() ) {
                if ( b.isLowSeqRevision() ) {
                    if ( a.lowSeq() != b.lowSeq() ) return a.lowSeq() < b.lowSeq();
                    ParsedSequenceID aInner(a.seq(), a.triggeredBy(), 0);
                    ParsedSequenceID bInner(b.seq(), b.triggeredBy(), 0);
                    return aInner.before(bInner);
                }
                if ( b.isTriggeredRevision() ) return a.lowSeq() < b.triggeredBy();
                /* b is simple seq*/
                return a.lowSeq() < b.seq();
            }

            if ( a.isTriggeredRevision() ) {
                if ( b.isLowSeqRevision() ) return a.triggeredBy() <= b.lowSeq();
                if ( b.isTriggeredRevision() ) {
                    if ( a.triggeredBy() != b.triggeredBy() ) return a.triggeredBy() < b.triggeredBy();
                    return a.seq() < b.seq();
                }
                /* b is simple */
                return a.triggeredBy() <= b.seq();
            }

            // a is simple revision
            if ( b.isLowSeqRevision() ) return a.seq() <= b.lowSeq();
            if ( b.isTriggeredRevision() ) return a.seq() < b.triggeredBy();
            /* both simple */
            return a.seq() < b.seq();
        }

      private:
        uint64_t _seq{0};          ///< The actual internal database sequence number.
        uint64_t _triggeredBy{0};  ///< Sequence# of the access-grant that triggered backfill (0 = none).
        uint64_t _lowSeq{0};       ///< Lowest contiguous sequence seen on the feed (0 = not present).
    };

}  // namespace litecore::repl
