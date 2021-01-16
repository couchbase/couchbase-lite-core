//
//  VersionVector.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/23/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "Base.hh"
#include <vector>
#include <list>
#include <iostream>
#include <optional>
#include <string_view>

namespace fleece {
    class Value;
    class Encoder;
}


namespace litecore {

    class VersionVector;

    /** A version's generation number: the number of times its author has changed the document. */
    typedef uint64_t generation;

    /** Identifier of a peer or server that created a version. A simple opaque 64-bit value. */
    struct peerID {
        uint64_t id = 0;
        bool operator==(peerID p) const noexcept FLPURE   {return id == p.id;}
        bool operator!=(peerID p) const noexcept FLPURE   {return id != p.id;}
    };

    /** A placeholder `peerID` representing the local peer, i.e. this instance of Couchbase Lite.
        This is needed since I won't have a real assigned peer ID until I talk to a server. */
    constexpr peerID kMePeerID  {0};

    /** The possible orderings of two VersionVectors. (Can be interpreted as two 1-bit flags.) */
    enum versionOrder {
        kSame        = 0,                   // Equal
        kOlder       = 1,                   // This one is older
        kNewer       = 2,                   // This one is newer
        kConflicting = kOlder | kNewer      // The vectors conflict
    };


    /** A single version identifier in a VersionVector.
        Consists of a peerID (author) and generation count.
        The local peer's ID is represented as 0 (`kMePeerID`) for simplicity and compactness.

        The absolute ASCII form of a Version is: <hex generation> '@' <hex peerID> .
        The relative form uses a "*" character for the peerID when it's equal to the local peer's ID.

        The binary form is the generation as a varint followed by the peerID as a varint. */
    class Version {
    public:
        Version(generation g, peerID p)         :_author(p), _gen(g) {validate();}

        /** Initializes from ASCII. Throws BadRevisionID if the string's not valid.
            If `myPeerID` is given, then the string is expected to be in absolute
            form, with no "*" allowed. myPeerID in the string will be changed to kMePeerID (0). */
        explicit Version(slice ascii, peerID myPeerID =kMePeerID);

        /** Initializes from binary. On return, `binaryP->buf` will point just past the last byte read. */
        explicit Version(slice *binaryP);

        /** The peer that created this version. */
        const peerID author() const             {return _author;}

        /** The generation count: the number of versions this peer has created. */
        generation gen() const                  {return _gen;}

        /** Max length of a Version in ASCII form. */
        static constexpr size_t kMaxASCIILength = 2 * 16 + 1;

        /** Converts the version to a human-readable string.
            When sharing a version with another peer, pass your actual peer ID in `myID`;
            then if `author` is kMePeerID it will be written as that ID.
            Otherwise it's written as '*'. */
        alloc_slice asASCII(peerID myID =kMePeerID) const;

        bool writeASCII(slice *buf, peerID myID =kMePeerID) const;
        bool writeBinary(slice *buf, peerID myID =kMePeerID) const;

        /** Convenience to compare two generations and return a versionOrder. */
        static versionOrder compareGen(generation a, generation b);

        /** Compares with a version vector, i.e. whether a vector with this as its current version
            is newer/older/same as the target vector. (Will never return kConflicting.) */
        versionOrder compareTo(const VersionVector&) const;

        bool operator== (const Version& v) const {
            return _gen == v._gen && _author == v._author;
        }

        bool operator < (const Version& v) const {
            return _gen < v._gen && _author == v._author;
        }

    private:
        void validate() const;

        peerID      _author;                // The ID of the peer who created this revision
        generation  _gen;                   // The number of times this peer edited this revision
    };


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
        VersionVector() { }

        /** Parses textual form from ASCII data.
            Throws BadRevisionID if the string's not valid.
            If `myPeerID` is given, then the string is expected to be in absolute
            form, with no "*" allowed. myPeerID in the string will be changed to kMePeerID (0). */
        void readASCII(slice asciiString, peerID myPeerID =kMePeerID);

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

        size_t count() const                                {return _vers.size();}
        bool empty() const                                  {return _vers.size() == 0;}
        const Version& operator[] (size_t i) const          {return _vers[i];}
        const Version& current() const                      {return _vers.at(0);}
        const std::vector<Version>& versions() const        {return _vers;}

        /** Returns the generation count for the given author. */
        generation genOfAuthor(peerID) const;
        generation operator[] (peerID author) const         {return genOfAuthor(author);}

        //---- Comparisons:

        /** Compares this vector to another. */
        versionOrder compareTo(const VersionVector&) const;

        bool operator == (const VersionVector& v) const     {return compareTo(v) == kSame;}
        bool operator != (const VersionVector& v) const     {return !(*this == v);}
        bool operator <  (const VersionVector& v) const     {return compareTo(v) == kOlder;}
        bool operator >  (const VersionVector& v) const     {return compareTo(v) == kNewer;}
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

        bool writeASCII(slice *buf, peerID myID =kMePeerID) const;
        size_t maxASCIILen() const;

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
        VersionVector(std::vector<Version>::const_iterator begin,
                      std::vector<Version>::const_iterator end)
        :_vers(begin, end)
        { }
#if DEBUG
        void validate() const;
#else
        void validate() const                               { }
#endif
        // Finds my version by this author and returns an iterator to it, else returns end()
        std::vector<Version>::iterator findPeerIter(peerID) const;

        std::vector<Version> _vers;          // versions, in order
    };

}
