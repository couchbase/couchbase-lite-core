//
// Version.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once

#include "VersionTypes.hh"
#include <optional>

namespace litecore {
    class HybridClock;
    class VersionVector;

    /** A single version identifier in a VersionVector.
        Consists of a SourceID (author) and logicalTime.
        The local peer's ID is represented as 0 (`kMeSourceID`) for simplicity and compactness.

        The absolute ASCII form of a Version is: <hex logicalTime> '@' <base64 SourceID> .
        The relative form uses a "*" character for the SourceID when it's equal to the local peer's ID.

        The binary form is the concatenation of time's and author's binary forms (see Version.cc) */
    class Version {
      public:
        /** Constructs a Version from a timestamp and peer ID. */
        Version(logicalTime t, SourceID p) : _author(p), _time(t) { validate(); }

#pragma mark - Accessors:

        /** The peer that created this version. */
        [[nodiscard]] SourceID author() const noexcept FLPURE { return _author; }

        /** The logical time at which this peer last updated the doc. */
        [[nodiscard]] logicalTime time() const noexcept FLPURE { return _time; }

#pragma mark - I/O:

        /** Initializes from ASCII. Throws BadRevisionID if the string's not valid.
            If `mySourceID` is given, the peer ID "*" will be converted to this ID instead of
            kMeSourceID. */
        explicit Version(slice ascii, SourceID mySourceID = kMeSourceID);

        /** Initializes from binary. On return, `binary.buf` will point just past the last byte read. */
        explicit Version(fleece::slice_istream& binary);

        /** Max length of a Version in ASCII form. */
        static constexpr size_t kMaxASCIILength = 16 + 1 + SourceID::kASCIILength;

        /** Parses an ASCII version string.
            @param ascii  The string to be parsed.
            @param mySourceID  If given, the peer ID "*" will be converted to this ID instead of
                            kMeSourceID.
            @returns  The parsed Version, or `nullopt` on failure. */
        [[nodiscard]] static std::optional<Version> readASCII(slice ascii, SourceID mySourceID = kMeSourceID);

        /** Converts the version to a human-readable string.
            When sharing a version with another peer, pass your actual peer ID in `myID`;
            then if `author` is kMeSourceID it will be written as that ID.
            Otherwise it's written as '*'. */
        [[nodiscard]] alloc_slice asASCII(SourceID myID = kMeSourceID) const;

        /** Writes the Version in ASCII form to a slice-stream.
            If `myID` is given, it will be written instead of a "*" for `kMeSourceID`. */
        [[nodiscard]] bool writeASCII(slice_ostream&, SourceID myID = kMeSourceID) const;

        /** Writes the Version in binary form to a slice-stream.
            If `myID` is given, it will be substituted for `kMeSourceID`. */
        [[nodiscard]] bool writeBinary(slice_ostream&, SourceID myID = kMeSourceID) const;

#pragma mark - Comparison:

        /** Convenience to compare two logicalTimes and return a versionOrder. */
        static versionOrder compare(logicalTime a, logicalTime b);

        /** Compares with a version vector, i.e. whether a vector with this as its current version
            is newer/older/same as the target vector. (Will never return kConflicting.) */
        [[nodiscard]] versionOrder compareTo(const VersionVector&) const;

        bool operator==(const Version& v) const { return _time == v._time && _author == v._author; }

        bool operator!=(const Version& v) const { return !(*this == v); }

        /// Version comparator function that sorts them by ascending author.
        static bool byAuthor(Version const& a, Version const& b) { return a.author() < b.author(); }

        /// Version comparator function that sorts them by ascending timestamp.
        /// If two timestamps are equal (very unlikely!) `byAuthor` is the tiebreaker.
        static bool byAscendingTimes(Version const& a, Version const& b) {
            return a.time() < b.time() || (a.time() == b.time() && !byAuthor(a, b));
        }

        /// Version comparator function that sorts them by descending timestamp
        /// (as in a VersionVector.)
        /// If two timestamps are equal (very unlikely!) `byAuthor` is the tiebreaker.
        static bool byDescendingTimes(Version const& a, Version const& b) {
            return a.time() > b.time() || (a.time() == b.time() && byAuthor(a, b));
        }

#pragma mark - Clock:

        /// Updates the clock, if necessary, so its `now` will be greater than this Version's time.
        /// (Equivalent to `clock.see(this.time())`.)
        /// @param  clock  The clock to update.
        /// @param anyone  If false (default), the clock is only updated if my author is kMeSourceID.
        /// @return  True on success, false if my timestamp is invalid.
        [[nodiscard]] bool updateClock(HybridClock& clock, bool anyone = false) const;

      private:
        friend class VersionVector;

        Version() = default;
        bool _readASCII(slice ascii) noexcept;
        void validate() const;

        // Only used by Version and VersionVector
        [[noreturn]] static void throwBadBinary();
        [[noreturn]] static void throwBadASCII(fleece::slice string = fleece::nullslice);

        SourceID    _author;  // The ID of the peer who created this revision
        logicalTime _time{};  // The logical timestamp of the revision
    };

}  // namespace litecore
