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

        The string form of a Version is: <hex generation> '@' (<hex peerID> | '*')
        where the '*' represents the "me" peer ID (0).

        The binary form is the generation as a varint followed by the peerID as a varint. */
    class Version {
    public:
        Version(generation g, peerID p)         :_author(p), _gen(g) {validate();}
        explicit Version(slice ascii);
        explicit Version(slice *binaryP);

        const peerID author() const             {return _author;}
        generation gen() const                  {return _gen;}

        // Max length of a Version in ASCII form.
        static constexpr size_t kMaxASCIILength = 2 * 16 + 1;

        bool writeBinary(slice *buf, peerID myID =kMePeerID) const;
        bool writeASCII(slice *buf, peerID myID =kMePeerID) const;
        alloc_slice asASCII(peerID myID =kMePeerID) const;

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
        /** Constructs an empty vector. */
        VersionVector() { }

        explicit VersionVector(const string &str)           {readASCII(str);}

        /** Parses textual form from ASCII data */
        void readASCII(const string&);

        /** Reads binary form. */
        void readBinary(slice binaryData);

        /** Reads just the current (first) Version from the binary form. */
        static Version readCurrentVersionFromBinary(slice binaryData);

        /** Sets the vector to empty. */
        void reset()                                        {_vers.clear();}

        /** True if the vector is non-empty. */
        explicit operator bool() const                      {return count() > 0;}

        size_t count() const                                {return _vers.size();}
        const Version& operator[] (size_t i) const          {return _vers[i];}
        const Version& current() const                      {return _vers.at(0);}
        const std::vector<Version>& versions() const        {return _vers;}

        /** Returns the generation count for the given author. */
        generation genOfAuthor(peerID) const;
        generation operator[] (peerID author) const         {return genOfAuthor(author);}

        /** Increments the generation count of the given author (or sets it to 1 if it didn't exist)
            and moves it to the start of the vector. */
        void incrementGen(peerID);

        /** Returns a new vector representing a merge of this vector and the argument.
            All the authors in both are present, with the larger of the two generations. */
        VersionVector mergedWith(const VersionVector&) const;

        /** Replaces the given peerID with kMePeerID ("*") in the vector. */
        void compactMyPeerID(peerID myID);

        /** Replaces kMePeerID ("*") with the given peerID in the vector. */
        void expandMyPeerID(peerID myID);

        /** Returns true if none of the versions' authors are "*". */
        bool isExpanded() const;

        //---- Conversions:

        /** Generates binary form. */
        fleece::alloc_slice asBinary(peerID myID = kMePeerID) const;

        /** Converts the vector to a human-readable string, replacing kMePeerID ("*") with the
            given peerID in the output. Use this when sharing a vector with another peer. */
        fleece::alloc_slice asASCII(peerID myID = kMePeerID) const;

        bool writeASCII(slice *buf, peerID myID =kMePeerID) const;
        size_t maxASCIILen() const;

        //---- Comparisons:

        /** Compares this vector to another. */
        versionOrder compareTo(const VersionVector&) const;

        bool operator == (const VersionVector& v) const     {return compareTo(v) == kSame;}
        bool operator != (const VersionVector& v) const     {return !(*this == v);}
        bool operator < (const VersionVector& v) const      {return compareTo(v) == kOlder;}
        bool operator > (const VersionVector& v) const      {return compareTo(v) == kNewer;}

        /** Compares with a single version, i.e. whether this vector is newer/older/same as a
            vector with the given current version. (Will never return kConflicting.) */
        versionOrder compareTo(const Version&) const;

        bool operator == (const Version& v) const           {return compareTo(v) == kSame;}
        bool operator >= (const Version& v) const           {return compareTo(v) != kOlder;}

#if 0
        // Methods that were implemented but untested and not in use...

        std::string canonicalString(peerID myPeerID) const;

        /** Given a string-encoded VersionVector, returns its current version. */
        static slice extractCurrentVersionFromString(slice versionVectorString) {
            return versionVectorString.upTo(versionVectorString.findByteOrEnd(','));
        }

        /** Populates an empty vector from a Fleece value. */
        void readFrom(fleece::Value);

        /** Writes a VersionVector to a Fleece encoder (as an array of alternating generations and
         peer IDs.) */
        void writeTo(fleece::Encoder&) const;
#endif

    private:
        std::vector<Version>::iterator findPeerIter(peerID);
        void append(const Version&);

        std::vector<Version> _vers;          // versions, in order
    };

}
