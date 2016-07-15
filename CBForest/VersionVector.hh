//
//  VersionVector.hh
//  CBForest
//
//  Created by Jens Alfke on 5/23/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef VersionVector_hh
#define VersionVector_hh

#include "slice.hh"
#include "RevID.hh"
#include <vector>
#include <list>
#include <iostream>

namespace fleece {
    class Value;
    class Encoder;
}

namespace cbforest {

    class VersionVector;

    typedef slice peerID;
    typedef uint64_t generation;

    extern const peerID kCASServerPeerID;   // "$"
    extern const peerID kMePeerID;          // "*"

    /** The possible orderings of two VersionVectors. */
    enum versionOrder {
        kSame        = 0,                   // Equal
        kOlder       = 1,                   // This one is older
        kNewer       = 2,                   // This one is newer
        kConflicting = kOlder | kNewer      // The vectors conflict
    };


    /** A single version identifier in a VersionVector.
        Consists of a peerID (author) and generation count.
        Does NOT own the storage of `author`! */
    struct version {
        peerID author;
        generation gen {0};

        static const size_t kMaxAuthorSize = 64;

        version()                               {}
        version(generation g, peerID p)         :author(p), gen(g) {validate();}
        version(slice string)                   :version(string, true) { }

        bool operator== (const version& v) const {
            return gen == v.gen && author == v.author;
        }

        alloc_slice asString() const;
//        revidBuffer asRevID() const         {return revidBuffer((unsigned)gen, author, kClockType);}

        /** The CAS counter of a version that comes from a CAS server.
            If author == kCASServerPeerID, returns the gen; else returns 0. */
        generation CAS() const;

        /** Convenience to compare two generations and return a versionOrder. */
        static versionOrder compareGen(generation a, generation b);

        /** Compares with a version vector, i.e. whether a vector with this as its current version
            is newer/older/same as the target vector. (Will never return kConflicting.) */
        versionOrder compareTo(const VersionVector&) const;

    private:
        void validate() const;
        friend class VersionVector;

        version(slice string, bool validateAuthor);
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
        VersionVector(VersionVector&&);

        VersionVector& operator=(const VersionVector&);

        /** Populates an empty vector from a Fleece value. */
        void readFrom(const fleece::Value*);

        void reset();

        size_t count() const                                {return _vers.size();}
        const version& operator[] (size_t i) const          {return _vers[i];}
        const version& current() const                      {return _vers.at(0);}
        const std::vector<version> versions() const         {return _vers;}

        generation genOfAuthor(peerID) const;
        generation operator[] (peerID author) const         {return genOfAuthor(author);}

        /** Increments the generation count of the given author (or sets it to 1 if it didn't exist)
            and moves it to the start of the vector. */
        void incrementGen(peerID);

        /** Replaces the given peerID with kMePeerID ("*") in the vector. */
        void compactMyPeerID(peerID myID);

        /** Replaces kMePeerID ("*") with the given peerID in the vector. */
        void expandMyPeerID(peerID myID);

        /** Has this vector been modified since it was created? */
        bool changed() const                                {return _changed;}

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
        versionOrder compareTo(const version&) const;

        bool operator == (const version& v) const           {return compareTo(v) == kSame;}
        bool operator >= (const version& v) const           {return compareTo(v) != kOlder;}

        /** Returns a new vector representing a merge of this vector and the argument.
            All the authors in both are present, with the larger of the two generations. */
        VersionVector mergedWith(const VersionVector&) const;

    private:
        std::vector<version>::iterator findPeerIter(peerID);
        alloc_slice copyAuthor(peerID);
        void append(version);
        friend class versionMap;

        alloc_slice _string;                        // The string I was parsed from
        std::vector<version> _vers;                 // versions, in order
        std::list<alloc_slice> _addedAuthors;       // storage space for added peerIDs
        bool _changed {false};                      // Changed since created?
    };


    // Some implementations of "<<" to write to ostreams and Fleece encoders: 

    static inline std::ostream& operator<< (std::ostream& o, const version &v) {
        return o << v.gen << "@" << (std::string)v.author;
    }
    
    static inline std::ostream& operator<< (std::ostream& o, const VersionVector &vv) {
        return o << (std::string)vv.asString();
    }
    
    static inline fleece::Encoder& operator<< (fleece::Encoder &encoder, const VersionVector &vv) {
        vv.writeTo(encoder);
        return encoder;
    }

}

#endif /* VersionVector_hh */
