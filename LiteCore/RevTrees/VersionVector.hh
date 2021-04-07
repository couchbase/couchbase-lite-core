//
//  VersionVector.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/23/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "Version.hh"
#include "SmallVector.hh"
#include <optional>

namespace litecore {

    /** A version vector: an array of version identifiers in reverse chronological order.
        Can be serialized either as a human-readable string or as binary data.
        The string format is comma-separated Version strings (see above).
        The binary format is consecutive binary Versions (see above). */
    class VersionVector {
    public:
        /** Returns a VersionVector parsed from ASCII; see `readASCII` for details. */
        static VersionVector fromASCII(slice asciiString, peerID myPeerID =kMePeerID) {
            VersionVector v;
            v.readASCII(asciiString, myPeerID);
            return v;
        }

        /** Returns a VersionVector parsed from binary data. */
        static VersionVector fromBinary(slice binary) {
            VersionVector v;
            v.readBinary(binary);
            return v;
        }

        /** Constructs an empty vector. */
        VersionVector() =default;

        /** Parses textual form from ASCII data. Overwrites any existing state.
            Throws BadRevisionID if the string's not valid.
            If `myPeerID` is given, then the string is expected to be in absolute
            form, with no "*" allowed. myPeerID in the string will be changed to kMePeerID (0). */
        void readASCII(slice asciiString, peerID myPeerID =kMePeerID);

        /** Assembles a version vector from its history, as a list of ASCII versions/vectors.
            This can take a few forms:
            - ["new version vector"]
            - ["new version", "parent version vector"]
            - ["new version", "parent version", "grandparent version" ...]
            Throws BadRevisionID if the history isn't in a form it understands. */
        void readHistory(const slice history[],
                         size_t historyCount,
                         peerID myPeerID =kMePeerID);

        /** Reads binary form. */
        void readBinary(slice binaryData);

        /** Reads just the current (first) Version from the binary form. */
        static Version readCurrentVersionFromBinary(slice binaryData);

        /** Sets the vector to empty. */
        void reset()                                        {_vers.clear();}

        /** True if the vector is non-empty. */
        explicit operator bool() const                      {return count() > 0;}

        using vec = fleece::smallVector<Version, 2>;

        size_t count() const                                {return _vers.size();}
        bool empty() const                                  {return _vers.size() == 0;}
        const Version& operator[] (size_t i) const          {return _vers[i];}
        const Version& current() const                      {return _vers.get(0);}
        const vec& versions() const                         {return _vers;}

        /** Returns the generation count for the given author. */
        generation genOfAuthor(peerID) const;
        generation operator[] (peerID author) const         {return genOfAuthor(author);}

        //---- Comparisons:

        /** Compares this vector to another. */
        versionOrder compareTo(const VersionVector&) const;

        /** Is this vector newer than the other vector, if you ignore the peerID `ignoring`? */
        bool isNewerIgnoring(peerID ignoring, const VersionVector &other) const;

        bool operator == (const VersionVector& v) const     {return compareTo(v) == kSame;}
        bool operator != (const VersionVector& v) const     {return !(*this == v);}
        bool operator <  (const VersionVector& v) const     {return compareTo(v) == kOlder;}
        bool operator >  (const VersionVector& v) const     {return v < *this;}
        bool operator <= (const VersionVector& v) const     {return compareTo(v) <= kOlder;}
        bool operator >= (const VersionVector& v) const     {return v <= *this;}

        /** Compares with a single version, i.e. whether this vector is newer/older/same as a
            vector with the given current version. (Will never return kConflicting.) */
        versionOrder compareTo(const Version&) const;

        bool operator == (const Version& v) const           {return compareTo(v) == kSame;}
        bool operator >= (const Version& v) const           {return compareTo(v) != kOlder;}

        //---- Conversions:

        /** Generates binary form. */
        fleece::alloc_slice asBinary(peerID myID = kMePeerID) const;

        /** Converts the vector to a human-readable string.
            When sharing a vector with another peer, pass your actual peer ID in `myID`;
            then occurrences of kMePeerID will be written as that ID.
            Otherwise they're written as '*'. */
        fleece::alloc_slice asASCII(peerID myID = kMePeerID) const;

        bool writeASCII(slice_stream&, peerID myID =kMePeerID) const;
        size_t maxASCIILen() const;

#if DEBUG
        std::string asString() const;
#endif

        //---- Expanding "*":

        /** Returns true if none of the versions' authors are "*". */
        bool isExpanded() const;

        /** Replaces kMePeerID ("*") with the given peerID in the vector. */
        void expandMyPeerID(peerID myID);

        /** Replaces the given peerID with kMePeerID ("*") in the vector. */
        void compactMyPeerID(peerID myID);

        //---- Operations:

        /** Increments the generation count of the given author (or sets it to 1 if it didn't exist)
            and moves it to the start of the vector. */
        void incrementGen(peerID);

        /** Truncates the vector to the maximum count `maxCount`. */
        void limitCount(size_t maxCount);

        /** Adds a version to the front of the vector.
            Any earlier version by the same author is removed.
            If there's an equal or newer version by the same author, the call fails and returns false. */
        bool add(Version);

        /** Adds a Version at the _end_ of the vector (the oldest position.) */
        void push_back(const Version&);

        /** Returns a new vector representing a merge of this vector and the argument.
            All the authors in both are present, with the larger of the two generations. */
        VersionVector mergedWith(const VersionVector&) const;

        //---- Deltas:

        /** Creates a VersionVector expressing the changes from an earlier VersionVector to this one.
            If the other vector is not earlier or equal, `nullopt` is returned. */
        std::optional<VersionVector> deltaFrom(const VersionVector &base) const;

        /** Applies a delta created by calling \ref deltaFrom on a newer VersionVector.
            if `D = B.deltaFrom(A)`, then `A.byApplyingDelta(D) == B`.
            If the delta is invalid, throws `BadRevisionID`.
            \warning If the delta was not created with this revision as a base, the result is undefined.
                    The method is likely to return an incorrect vector, not throw an exception. */
        VersionVector byApplyingDelta(const VersionVector &delta) const;

    private:

        VersionVector(vec::const_iterator begin,
                      vec::const_iterator end)
        :_vers(begin, end)
        { }
#if DEBUG
        void validate() const;
#else
        void validate() const                               { }
#endif
        // Finds my version by this author and returns an iterator to it, else returns end()
        vec::iterator findPeerIter(peerID) const;

        vec _vers;          // versions, in order
    };

}
