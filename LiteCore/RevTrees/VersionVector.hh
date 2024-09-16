//
//  VersionVector.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/23/16.
//  Copyright 2016-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once

#include "Version.hh"
#include "SmallVector.hh"
#include <optional>

namespace litecore {
    class HybridClock;

    /** A version vector: an array of Versions in reverse chronological order (more or less.)

        - The first Version is the **current** one that identifies the current revision.
          It's the one used as a document's `revid`.
        - The rest are **previous** versions that were once the current one. These have a well-
          defined causal ordering with the current version, i.e. they all "happened before it."
          They aren't needed in normal use, only to reconcile different revisions and
          decide which one is newer or if they conflict. That's the same sort of role as the
          revision history/tree used to have.
        - Two of the non-current versions may be identified as **merge versions**: these were the
          current versions of the conflicting documents that were merged to create the current one.
          Marking these makes it possible to tell that two VersionVectors (and their associated
          revisions) result from merging the same conflict.

        A VersionVector can be serialized either as a human-readable string or as binary data.

        The ASCII format is a sequence of ASCII Versions (see `Version` class docs);
        the delimiter is a `,`, except for a `;` that separates the current and merge version(s)
        from older ones. Spaces may follow a delimiter. As a special case there may be a trailing
        `;` after the last Version to indicate that there are no non-merge versions.

        The binary format is consecutive binary Versions (see `Version` class docs.)
        Each encoded Version's auxiliary `current` flag indicates whether it's a current/merge
        version or an older one; the current/merge ones come first.*/
    class VersionVector {
      public:
        using vec = fleece::smallVector<Version, 2>;

#pragma mark - Creating / Parsing:

        /// Returns a VersionVector parsed from ASCII; see `readASCII` for details.
        [[nodiscard]] static VersionVector fromASCII(slice asciiString, SourceID mySourceID = kMeSourceID) {
            VersionVector v;
            v.readASCII(asciiString, mySourceID);
            return v;
        }

        /** Returns a VersionVector parsed from binary data.
            Throws BadRevisionID if parsing fails.  */
        [[nodiscard]] static VersionVector fromBinary(slice binary) {
            VersionVector v;
            v.readBinary(binary);
            return v;
        }

        /// Constructs an empty vector.
        VersionVector() = default;

        /** Parses textual form from ASCII data. Overwrites any existing state.
            Throws BadRevisionID if the string's not valid.
            If `mySourceID` is given, then the string is expected to be in absolute
            form, with no "*" allowed. mySourceID in the string will be changed to kMeSourceID (0). */
        void readASCII(slice asciiString, SourceID mySourceID = kMeSourceID);

        /** Reads binary form.  Overwrites any existing state.
            Throws BadRevisionID if the data's not valid.*/
        void readBinary(slice binaryData);

        /// Reads just the current (first) Version from the ASCII form.
        [[nodiscard]] static std::optional<Version> readCurrentVersionFromASCII(slice asciiData);

        /// Reads just the current (first) Version from the binary form.
        [[nodiscard]] static Version readCurrentVersionFromBinary(slice binaryData);

        /// Sets the vector to empty.
        void clear() {
            _vers.clear();
            _nCurrent = 0;
        }

#pragma mark - Accessors:

        /// True if the vector is non-empty.
        explicit operator bool() const { return count() > 0; }

        /// True if the vector is empty.
        [[nodiscard]] bool empty() const { return _vers.empty(); }

        /// The number of Versions.
        [[nodiscard]] size_t count() const { return _vers.size(); }

        /// The Version at an index; 0 is current.
        const Version& operator[](size_t i) const { return _vers[i]; }

        /// The current version. Throws an exception if empty.
        [[nodiscard]] const Version& current() const { return _vers.get(0); }

        /// The array of Versions, as a `smallVector<Version>`.
        [[nodiscard]] const vec& versions() const { return _vers; }

        /// True if the vector contains a Version with the given author.
        bool contains(SourceID author) const { return timeOfAuthor(author) != logicalTime::none; }

        /// Returns the logical timestamp for the given author, or else logicalTime::none. */
        [[nodiscard]] logicalTime timeOfAuthor(SourceID) const;

        /// Returns the logical timestamp for the given author, or else logicalTime::none. */
        logicalTime operator[](SourceID author) const { return timeOfAuthor(author); }

#pragma mark - Comparisons:

        /// Compares this vector to another.
        [[nodiscard]] versionOrder compareTo(const VersionVector&) const;

        /// Is this vector newer than the other vector, if you ignore the SourceID `ignoring`?
        [[nodiscard]] bool isNewerIgnoring(SourceID ignoring, const VersionVector& other) const;

        // Comparison operators:
        bool operator==(const VersionVector& v) const { return compareTo(v) == kSame; }

        bool operator!=(const VersionVector& v) const { return !(*this == v); }

        bool operator<(const VersionVector& v) const { return compareTo(v) == kOlder; }

        bool operator>(const VersionVector& v) const { return v < *this; }

        bool operator<=(const VersionVector& v) const { return compareTo(v) <= kOlder; }

        bool operator>=(const VersionVector& v) const { return v <= *this; }

        /// A made-up comparison operator for 'conflicts with':
        bool operator%(const VersionVector& v) const { return compareTo(v) == kConflicting; }

        /** Compares with a single version, i.e. whether this vector is newer/older/same as a
            vector with the given current version. (Will never return kConflicting.) */
        [[nodiscard]] versionOrder compareTo(const Version&) const;

        bool operator==(const Version& v) const { return compareTo(v) == kSame; }

        bool operator>=(const Version& v) const { return compareTo(v) != kOlder; }

        using CompareBySourceFn = function_ref<bool(SourceID, logicalTime, logicalTime)>;

        /** For each SourceID found in either `this` or `other`, calls the callback with that ID
            and its timestamps from `this` and `other` (`none` if not present.)
            If the callback returns false, stops the iteration and returns false. */
        static bool compareBySource(VersionVector const& v1, VersionVector const& v2, CompareBySourceFn callback);

#pragma mark - Conversions:

        /// Generates binary form.
        [[nodiscard]] fleece::alloc_slice asBinary(SourceID myID = kMeSourceID) const;

        /** Converts the vector to a human-readable string.
            When sharing a vector with another peer, pass your actual peer ID in `myID`;
            then occurrences of kMeSourceID will be written as that ID.
            Otherwise they're written as '*'. */
        [[nodiscard]] fleece::alloc_slice asASCII(SourceID myID = kMeSourceID) const;

        /// Same as \ref asASCII but returns a `std::string`, for convenience.
        [[nodiscard]] std::string asString(SourceID myID = kMeSourceID) const;

        /// Writes vector in ASCII form to a slice-stream.
        /// If `myID` is given, occurrences of kMeSourceID will be written as that ID.
        bool writeASCII(slice_ostream&, SourceID myID = kMeSourceID) const;

        /// The maximum possible length in bytes of this vector's ASCII form.
        [[nodiscard]] size_t maxASCIILen() const;

#pragma mark - Expanding "*":

        /** Returns true if none of the versions' authors are "*" (`kMeSourceID`). */
        [[nodiscard]] bool isAbsolute() const;

        /** Replaces kMeSourceID ("*") with the given SourceID in the vector. */
        void makeAbsolute(SourceID myID);

        /** Replaces the given SourceID with kMeSourceID ("*") in the vector. */
        void makeLocal(SourceID myID);

#pragma mark - Operations:

        /** Updates/creates the Version for an author, assigning it a newer logical time,
            and moves it to the start of the vector.
            `currentVersions` is reset to 1 (i.e. no merges.) */
        void addNewVersion(HybridClock&, SourceID = kMeSourceID);

        /** Truncates the vector by removing the oldest Versions.
            It will never remove current or merged Versions.
            @param maxCount  The number of Versions you want to keep.
            @param before  If given, only Versions older than this will be removed; this means
                            more than `maxCount` may remain. */
        void prune(size_t maxCount, logicalTime before = logicalTime::endOfTime);

        /** Adds a version to the front of the vector, making it current.
            Any earlier version by the same author is removed.
            `currentVersions` is reset to 1 (i.e. no merges.)
            If there's an equal or newer version by the same author, throws. */
        void add(Version);

        /// Updates the HybridClock, if necessary, so its `now` will be greater than any of this
        /// vector's versions' times.
        /// @param  clock  The clock to update.
        /// @param anyone  If false (default), ignores versions not by kMeSourceID.
        /// @return  True on success, false if a Version has an invalid timestamp.
        [[nodiscard]] bool updateClock(HybridClock& clock, bool anyone = false) const;

#pragma mark - Conflict Resolution:

        /** Returns a new vector representing a merge of two conflicting vectors.
            - All the authors in both are present, with the larger of the two timestamps.
            - The conflicting versions are moved to the front and marked as merges.
            - A new version for "*" is prepended at the front.
            - This operation is commutative.*/
        [[nodiscard]] static VersionVector merge(const VersionVector& v1, const VersionVector& v2, HybridClock& clock);

        /// True if this vector is the direct result of merging conflicting versions.
        bool isMerge() const { return _nCurrent > 1; }

        /** The number of Versions that are current or merges. These always come first.
            The first Version in a vector is always current, so this property is always ≥ 1 unless
            the vector is empty. If it's > 1, the versions after the first are merges. */
        size_t currentVersions() const { return _nCurrent; }

        /// Returns the merged conflicting versions in a merge vector. There are usually two.
        /// Returns an empty vector if this is not a merge.
        vec mergedVersions() const;

        /// True if both vectors are merges and have the same mergedVersions.
        bool mergesSameVersions(VersionVector const&) const;

#pragma mark - Deltas:

        /** Creates a VersionVector expressing the changes from an earlier VersionVector to this one.
            If the other vector is not earlier or equal, `nullopt` is returned. */
        [[nodiscard]] std::optional<VersionVector> deltaFrom(const VersionVector& base) const;

        /** Applies a delta created by calling \ref deltaFrom on a newer VersionVector.
            if `D = B.deltaFrom(A)`, then `A.byApplyingDelta(D) == B`.
            If the delta is invalid, throws `BadRevisionID`.
            \warning If the delta was not created with this revision as a base, the result is undefined.
                    The method is likely to return an incorrect vector, not throw an exception. */
        [[nodiscard]] VersionVector byApplyingDelta(const VersionVector& delta) const;


      private:
        explicit VersionVector(vec&& v, size_t nCur) : _vers(std::move(v)), _nCurrent(nCur) {}

        explicit VersionVector(vec&& v) : VersionVector(std::move(v), !v.empty()) {}

        VersionVector(vec::const_iterator begin, vec::const_iterator end)
            : _vers(begin, end), _nCurrent(!_vers.empty()) {}

        static fleece::slice_istream openBinary(slice data);
        // Finds my version by this author and returns an iterator to it, else returns end()
        [[nodiscard]] vec::iterator findPeerIter(SourceID) const;
        [[nodiscard]] bool          replaceAuthor(SourceID old, SourceID nuu) noexcept;
        void                        _removeAuthor(SourceID);
        void                        _add(Version const&);
        void                        validate() const;
        vec                         versionsBySource() const;

        vec         _vers;          // versions, in order from latest to oldest.
        size_t      _nCurrent = 0;  // Number of current/merged versions including the first
        alloc_slice _revID;         // legacy (tree-based) revision ID, if any
    };

}  // namespace litecore
