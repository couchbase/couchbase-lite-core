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

namespace fleece {
    class Value;
    class Encoder;
}


namespace litecore {

    class VersionVector;

    typedef slice peerID;
    typedef uint64_t generation;

    constexpr peerID kCASServerPeerID = {"$", 1};
    constexpr peerID kMePeerID        = {"*", 1};

    /** The possible orderings of two VersionVectors. */
    enum versionOrder {
        kSame        = 0,                   // Equal
        kOlder       = 1,                   // This one is older
        kNewer       = 2,                   // This one is newer
        kConflicting = kOlder | kNewer      // The vectors conflict
    };


    /** A single version identifier in a VersionVector.
        Consists of a peerID (author) and generation count.
        Does NOT own the storage of `author` -- its owning VersionVector does. */
    class Version {
    public:
        static const size_t kMaxAuthorSize = 64;

        /** Creates a valid peerID from binary data (like a raw digest or something.) */
        static alloc_slice peerIDFromBinary(slice binaryPeerID);

        Version(generation g, peerID p)         :_author(p), _gen(g) {validate();}
        explicit Version(slice string);

        peerID author() const                   {return _author;}
        generation gen() const                  {return _gen;}

        bool operator== (const Version& v) const {
            return _gen == v._gen && _author == v._author;
        }

        alloc_slice asString() const;

        bool isMerge() const                    {return _gen == 0;}

        /** The CAS counter of a version that comes from a CAS server.
            If author == kCASServerPeerID, returns the gen; else returns 0. */
        generation CAS() const;

        /** Convenience to compare two generations and return a versionOrder. */
        static versionOrder compareGen(generation a, generation b);

        /** Compares with a version vector, i.e. whether a vector with this as its current version
            is newer/older/same as the target vector. (Will never return kConflicting.) */
        versionOrder compareTo(const VersionVector&) const;

    private:
        friend class VersionVector;

        Version()                               {}
        void validate() const;

        peerID      _author;        // The ID of the peer who created this revision
        generation  _gen {0};       // The number of times this peer edited this revision
    };


    /** A version vector: an array of version identifiers in reverse chronological order.
        Can be serialized either as a human-readable string or as a binary Fleece value. */
    class VersionVector {
    public:
        /** Constructs an empty vector. */
        VersionVector() { }

        /** Parses version vector from string. Throws BadVersionVector if string is invalid.
            The input slice is not needed after the constructor returns. */
        explicit VersionVector(slice string);

        /** Parses version vector from Fleece value previously written by the overloaded "<<"
            operator. The Value needs to remain valid for the lifetime of the VersionVector. */
        explicit VersionVector(const fleece::Value*);

        VersionVector(const VersionVector&);
        VersionVector(VersionVector&&) noexcept;
        VersionVector& operator=(const VersionVector&);

        /** Populates an empty vector from a Fleece value.
            The vector will contain pointers into the Fleece data! */
        void readFrom(const fleece::Value*);

        /** Sets the vector to empty. */
        void reset();

        /** True if the vector is non-empty. */
        explicit operator bool() const                      {return count() > 0;}

        size_t count() const                                {return _vers.size();}
        const Version& operator[] (size_t i) const          {return _vers[i];}
        const Version& current() const                      {return _vers.at(0);}
        const std::vector<Version> versions() const         {return _vers;}

        /** Returns the generation count for the given author. */
        generation genOfAuthor(peerID) const;
        generation operator[] (peerID author) const         {return genOfAuthor(author);}

        /** Increments the generation count of the given author (or sets it to 1 if it didn't exist)
            and moves it to the start of the vector. */
        void incrementGen(peerID);

        /** Replaces the given peerID with kMePeerID ("*") in the vector. */
        void compactMyPeerID(peerID myID);

        /** Replaces kMePeerID ("*") with the given peerID in the vector. */
        void expandMyPeerID(peerID myID);

        /** Returns true if none of the versions' authors are "*". */
        bool isExpanded() const;

        /** Converts the vector to a human-readable string. */
        std::string asString() const;

        /** Converts the vector to a human-readable string, replacing kMePeerID ("*" with the
            given peerID in the output. Use this when sharing a vector with another peer. */
        std::string exportAsString(peerID myID) const;

        /** Writes a VersionVector to a Fleece encoder (as an array of alternating peer IDs and
            generation numbers.) */
        void writeTo(fleece::Encoder&) const;

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

        /** Returns a new vector representing a merge of this vector and the argument.
            All the authors in both are present, with the larger of the two generations. */
        VersionVector mergedWith(const VersionVector&) const;

        std::string canonicalString(peerID myPeerID) const;
        void insertMergeRevID(peerID myPeerID, slice revisionBody);

    private:
        std::vector<Version>::iterator findPeerIter(peerID);
        slice copyAuthor(peerID);
        void append(Version);
        friend class versionMap;

        alloc_slice             _string;        // The string I was parsed from (if any)
        std::vector<Version>    _vers;          // versions, in order
        std::list<alloc_slice>  _addedAuthors;  // storage space for added peerIDs
    };


    // Some implementations of "<<" to write to ostreams and Fleece encoders: 

    /** Writes a `version` to a stream.
        Note: This does not replace "*" with the local author's ID! */
    std::ostream& operator<< (std::ostream& o, const Version &v);

    /** Writes a VersionVector to a stream.
        Note: This does not replace "*" with the local author's ID! */
    static inline std::ostream& operator<< (std::ostream& o, const VersionVector &vv) {
        return o << (std::string)vv.asString();
    }

    /** Writes a VersionVector to a Fleece encoder. */
    static inline fleece::Encoder& operator<< (fleece::Encoder &encoder, const VersionVector &vv) {
        vv.writeTo(encoder);
        return encoder;
    }

}
