//
// Version.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//

#pragma once

#include "Base.hh"
#include <optional>

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

    /** The possible orderings of two Versions or VersionVectors. (Can be interpreted as two 1-bit flags.) */
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

        static std::optional<Version> readASCII(slice ascii, peerID myPeerID =kMePeerID);

        /** Converts the version to a human-readable string.
            When sharing a version with another peer, pass your actual peer ID in `myID`;
            then if `author` is kMePeerID it will be written as that ID.
            Otherwise it's written as '*'. */
        alloc_slice asASCII(peerID myID =kMePeerID) const;

        bool writeASCII(slice_stream&, peerID myID =kMePeerID) const;
        bool writeBinary(slice_stream&, peerID myID =kMePeerID) const;

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


        // Only used by Version and VersionVector
        static void throwBadBinary();
        static void throwBadASCII(fleece::slice string = fleece::nullslice);

    private:
        Version() =default;
        bool _readASCII(slice ascii) noexcept;
        void validate() const;

        peerID      _author;                // The ID of the peer who created this revision
        generation  _gen;                   // The number of times this peer edited this revision
    };

}
