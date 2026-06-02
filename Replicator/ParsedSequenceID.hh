#pragma once
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

        ParsedSequenceID(uint64_t seq, uint64_t triggeredBy, uint64_t lowSeq) : _seq(seq), _triggeredBy(triggeredBy), _lowSeq(lowSeq) {}

        [[nodiscard]] uint64_t seq()         const { return _seq; }
        [[nodiscard]] uint64_t triggeredBy() const { return _triggeredBy; }
        [[nodiscard]] uint64_t lowSeq()      const { return _lowSeq; }

        [[nodiscard]] bool isLowSeqFormat()    const { return _lowSeq > 0; }
        [[nodiscard]] bool isBackfillFormat()  const { return _lowSeq == 0 && _triggeredBy > 0; }
        [[nodiscard]] bool isSimpleRevision()  const { return _lowSeq == 0 && _triggeredBy == 0; }

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
            if ( str.empty() ) return false;

            // Split into up to 4 tokens by ':' (4th would mean invalid).
            uint64_t values[3] = {};
            bool     empty[3]  = {};
            int      count     = 0;
            size_t   start     = 0;

            for ( size_t i = 0; i <= str.size(); ++i ) {
                if ( i == str.size() || str[i] == ':' ) {
                    if ( count >= 3 ) return false;  // Too many components.
                    auto part = str.substr(start, i - start);
                    if ( part.empty() ) {
                        empty[count] = true;
                        values[count] = 0;
                    } else {
                        // Reject negative values explicitly — std::stoull wraps them silently.
                        if ( part[0] == '-' ) return false;
                        try {
                            size_t pos;
                            values[count] = std::stoull(part, &pos);
                            if ( pos != part.size() ) return false;  // Not a pure integer.
                        } catch ( ... ) { return false; }
                    }
                    ++count;
                    start = i + 1;
                }
            }

            out = {};
            switch ( count ) {
                case 1:
                    if ( empty[0] ) return false;
                    out = ParsedSequenceID(values[0], 0, 0);
                    return true;
                case 2:
                    if ( empty[0] || empty[1] ) return false;
                    out = ParsedSequenceID(values[1], values[0], 0);
                    return true;
                case 3:
                    if ( empty[0] || empty[2] ) return false;
                    out = ParsedSequenceID(values[2], values[1], values[0]);
                    return true;
                default:
                    return false;
            }
        }


        [[nodiscard]] bool before(const ParsedSequenceID& seqID2) const {
            const auto& a = *this;
            const auto& b = seqID2;

            if ( a.isLowSeqFormat() ) {
                if ( b.isLowSeqFormat() ) {
                    if ( a.lowSeq() != b.lowSeq() ) return a.lowSeq() < b.lowSeq();
                    ParsedSequenceID aInner(a.seq(), a.triggeredBy(), 0);
                    ParsedSequenceID bInner(b.seq(), b.triggeredBy(), 0);
                    return aInner.before(bInner);
                }
                if ( b.isBackfillFormat() ) return a.lowSeq() < b.triggeredBy();
                /* b is simple seq*/
                return a.lowSeq() < b.seq();
            }

            if ( a.isBackfillFormat() ) {
                if ( b.isLowSeqFormat() )  return a.triggeredBy() <= b.lowSeq();
                if ( b.isBackfillFormat() ) {
                    if ( a.triggeredBy() != b.triggeredBy() ) return a.triggeredBy() < b.triggeredBy();
                    return a.seq() < b.seq();
                }
                /* b is simple */
                return a.triggeredBy() <= b.seq();
            }

            // A is Simple revision
            if ( b.isLowSeqFormat() )  return a.seq() <= b.lowSeq();
            if ( b.isBackfillFormat() ) return a.seq() < b.triggeredBy();
            /* both simple */
            return a.seq() < b.seq();
        }

      private:
        uint64_t _seq{0};          ///< The actual internal database sequence number.
        uint64_t _triggeredBy{0};  ///< Sequence# of the access-grant that triggered backfill (0 = none).
        uint64_t _lowSeq{0};       ///< Lowest contiguous sequence seen on the feed (0 = not present).
    };

}  // namespace litecore::repl

