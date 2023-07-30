//
// VersionTypes.hh
//
// Copyright © 2023 Couchbase. All rights reserved.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "Base.hh"
#include <array>
#include <stdexcept>

namespace fleece {
    struct slice_istream;
    class slice_ostream;
}  // namespace fleece

namespace litecore {

    // Types used by VersionVector, Version and HybridClock.


    /** A version's logical timestamp indicating when a peer last made a change.
        This is not necessarily a real time; for most purposes it suffices that it just increases
        monotonically for a given peer when they make changes.
        In practice it's a "Hybrid Logical Timestamp" and close to real-time; see HybridClock.hh.*/
    enum class logicalTime : uint64_t { none = 0, endOfTime = UINT64_MAX };

    /** Unique 128-bit identifier of a client or server that created a Version.
        An all-zeroes instance (kMeSourceID) stands for the local client's ID. */
    class SourceID {
      public:
        static constexpr size_t kASCIILength = 22;

        constexpr SourceID() noexcept : SourceID(0, 0) {}

        constexpr SourceID(uint64_t w1, uint64_t w2) noexcept : _w1{w1}, _w2{w2} {}

        constexpr explicit SourceID(std::initializer_list<uint8_t> bytes) : _bytes{} {
            if ( bytes.size() > _bytes.size() ) throw std::invalid_argument("too many bytes");
            for ( size_t i = 0; i < bytes.size(); ++i ) _bytes[i] = bytes.begin()[i];
        }

        /// Formats a SourceID as a string (in base64.) Does not do the "*" shortcut.
        std::string asASCII() const;

        bool writeASCII(fleece::slice_ostream&) const;
        bool readASCII(fleece::slice);

        /// Writes the SourceID to a binary stream, plus a `current` flag used by VersionVector.
        bool writeBinary(fleece::slice_ostream&, bool current) const;
        /// Reads the SourceID from a binary stream, plus a `current` flag used by VersionVector.
        bool readBinary(fleece::slice_istream&, bool* current);

        constexpr std::array<uint8_t, 16>& bytes() noexcept FLPURE { return _bytes; }

        constexpr std::array<uint8_t, 16> const& bytes() const noexcept FLPURE { return _bytes; }

        constexpr bool isMe() const noexcept FLPURE { return (_w1 | _w2) == 0; }

        friend constexpr bool operator==(SourceID const& a, SourceID const& b) noexcept FLPURE {
            return a._w1 == b._w1 && a._w2 == b._w2;
        }

        friend constexpr bool operator!=(SourceID const& a, SourceID const& b) noexcept FLPURE { return !(a == b); }

        friend bool operator<(SourceID const& a, SourceID const& b) noexcept FLPURE { return a._bytes < b._bytes; }

      private:
        union {
            struct {
                uint64_t _w1, _w2;
            };  // (mostly an optimization for comparisons)

            std::array<uint8_t, 16> _bytes;
        };
    };

    /** A placeholder `SourceID` representing the local peer, i.e. this instance of Couchbase Lite.
        Its binary value is all zeroes; it encodes to ASCII as `*` and binary as a single 00 byte.
        This not only saves space, it also lets us use version vectors before we know what our
        real peer ID is, since it might be assigned by a server. In practice the real local peer
        ID (`DatabaseImpl::mySourceID()` or `C4Database::getSourceID()`) is only used during
        replication. */
    constexpr SourceID kMeSourceID;

    /** A `SourceID` used to represent a version created during upgrade of a database. */
    constexpr SourceID kLegacyRevSourceID{0x1e /*... rest is 00 */};

    /** The possible orderings of two Versions or VersionVectors.
        (Can be interpreted as two 1-bit flags.) */
    enum versionOrder {
        kSame        = 0,               // Equal
        kOlder       = 1,               // This one is older
        kNewer       = 2,               // This one is newer
        kConflicting = kOlder | kNewer  // The vectors conflict
    };

}  // namespace litecore
