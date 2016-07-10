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

    typedef slice peerID;
    typedef uint64_t generation;

    extern const peerID kCASServerPeerID;   // "$"
    extern const peerID kMePeerID;          // "*"

    /** A single version identifier in a vector. Consists of a peerID (author) and generation count.
        This is equivalent to a clock-style revid. */
    struct version {
        peerID author;
        generation gen {0};

        static const size_t kMaxAuthorSize = 64;

        version()                               {}
        version(generation g, peerID p)         :author(p), gen(g) {validate();}
        version(slice string)                   :version(string, true) { }
        void validate() const;

        bool operator== (const version& v) const {
            return gen == v.gen && author == v.author;
        }

        generation CAS() const;

        alloc_slice asString() const;
        revidBuffer asRevID() const         {return revidBuffer((unsigned)gen, author, kClockType);}

    private:
        friend class versionVector;

        version(slice string, bool validateAuthor);
    };


    /** A version vector: an array of version identifiers in reverse chronological order.
        Can be serialized either as a human-readable string or as a binary Fleece value. */
    class versionVector {
    public:
        /** Constructs an empty vector. */
        versionVector() { }

        /** Parses version vector from string. Throws BadVersionVector if string is invalid.
            The input slice is not needed after the constructor returns. */
        explicit versionVector(slice string);

        /** Parses version vector from Fleece value previously written by the overloaded "<<"
            operator. The Value needs to remain valid for the lifetime of the versionVector. */
        explicit versionVector(const fleece::Value*);

        /** Populates an empty vector from a Fleece value. */
        void readFrom(const fleece::Value*);

        size_t count() const                                {return _vers.size();}
        const version& operator[] (size_t i) const          {return _vers[i];}
        const version& current() const                      {return _vers[0];}
        const std::vector<version> versions() const         {return _vers;}

        generation genOfAuthor(peerID) const;
        generation operator[] (peerID author) const         {return genOfAuthor(author);}

        /** Is this a version from a CAS server? 
            Tests whether the first component's author is kCASServerPeerID ("$"). */
        bool isFromCASServer() const;

        /** Returns the generation number of kCASServerPeerID ("$"). */
        generation CAS() const                              {return genOfAuthor(kCASServerPeerID);}

        /** Sets the server CAS value, assigning it to the generation of kCASServerPeerID,
            and moves that component to the start.
            Does nothing and returns false if the generation is already >= the new value. */
        bool setCAS(generation);

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
        alloc_slice asString() const;
        explicit operator slice() const                     {return asString();}

        /** Writes a versionVector to a Fleece encoder (as an array of alternating peer IDs and
            generation numbers.) */
        void writeTo(fleece::Encoder&) const;

        enum order {
            kSame        = 0,                   // Equal
            kOlder       = 1,                   // This one is older
            kNewer       = 2,                   // This one is newer
            kConflicting = kOlder | kNewer      // The vectors conflict
        };

        /** Compares this vector to another. */
        order compareTo(const versionVector&) const;

        bool operator == (const versionVector& v) const     {return current() == v.current();}
        bool operator < (const versionVector& v) const      {return compareTo(v) == kOlder;}
        bool operator > (const versionVector& v) const      {return compareTo(v) == kNewer;}

        /** Returns a new vector representing a merge of this vector and the argument.
            All the authors in both are present, with the larger of the two generations. */
        versionVector mergedWith(const versionVector&) const;

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
    
    static inline std::ostream& operator<< (std::ostream& o, const versionVector &vv) {
        return o << (std::string)vv.asString();
    }
    
    static inline fleece::Encoder& operator<< (fleece::Encoder &encoder, const versionVector &vv) {
        vv.writeTo(encoder);
        return encoder;
    }

}

#endif /* VersionVector_hh */
