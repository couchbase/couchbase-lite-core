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

namespace cbforest {

    typedef slice peerID;
    typedef uint64_t generation;

    /** A single version identifier in a vector. Consists of a peerID (author) and generation count.
        This is equivalent to a clock-style revid. */
    struct version {
        peerID author;
        generation gen {0};

        static const size_t kMaxAuthorSize = 64;

        version()                               {}
        version(generation g, peerID p)         :author(p), gen(g) { }
        version(slice string)                   :version(string, true) { }
        void validate() const;

        bool operator== (const version& v) const {
            return gen == v.gen && author == v.author;
        }

        revidBuffer asRevID() const         {return revidBuffer((unsigned)gen, author, kClockType);}

    private:
        friend class versionVector;

        version(slice string, bool validateAuthor);
    };


    /** A version vector: an array of version identifiers in reverse chronological order. */
    class versionVector {
    public:
        versionVector() { }

        /** Parses version vector from string. Throws BadVersionVector if string is invalid. */
        explicit versionVector(slice string);

        size_t count() const                                {return _vers.size();}
        const version& operator[] (size_t i) const          {return _vers[i];}
        const version& current() const                      {return _vers[0];}

        generation genOfAuthor(peerID) const;
        generation operator[] (peerID author) const         {return genOfAuthor(author);}

        void incrementGen(peerID);

        void append(version);

        alloc_slice asString() const;

        explicit operator slice() const                     {return asString();}

        enum order {
            kSame        = 0,                   // Equal
            kOlder       = 1,                   // This one is older
            kNewer       = 2,                   // This one is newer
            kConflicting = kOlder | kNewer      // The vectors conflict
        };

        order compareTo(const versionVector&) const;

        bool operator == (const versionVector& v) const     {return current() == v.current();}
        bool operator < (const versionVector& v) const      {return compareTo(v) == kOlder;}
        bool operator > (const versionVector& v) const      {return compareTo(v) == kNewer;}

        versionVector mergedWith(const versionVector&) const;

    private:
        std::vector<version>::iterator findPeerIter(peerID);
        alloc_slice copyAuthor(peerID);
        friend class versionMap;

        alloc_slice _string;
        std::vector<version> _vers;
        std::list<alloc_slice> _addedAuthors;      // storage space for added peerIDs
        bool _changed {false};
    };

}

#endif /* VersionVector_hh */
