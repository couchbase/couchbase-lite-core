//
// RevID.hh
//
// Copyright (c) 2014 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "Base.hh"

namespace litecore {
    class Version;
    class VersionVector;

    /** A compressed revision ID. 
        Since this is based on slice, it doesn't own the memory it points to.

        There are two types of revision IDs: digests and versions.

        **Digest form** is/was used by the revision-tree versioning system in Couchbase Mobile 1 and 2.
        It consists of a generation count and an MD5 or SHA-1 digest.
        - The ASCII form looks like "123-cafebabedeadbeefdeadfade".
        - The binary form consists of the generation as a varint, followed by the digest as raw binary.

        **Version form** is used by the version-vector system in [the upcoming] Couchbase Mobile 3.
        It consists of a generation count and an integer "source ID" (or "peer ID".) The source ID zero is
        reserved to mean "the local device/database".
        - The ASCII form looks like "7b@cafebabe" -- note that the generation is hex, not decimal!
          The source ID zero is represented as a '*' character, e.g. "7b-*".
        - The binary form consists of a zero byte, the generation as a varint, and the source as a varint.
          (The leading zero is to distinguish it from the digest form.)
        The version form is also represented by the `Version` class.

        PLEASE NOTE: A revid in version form can store an entire version vector, since that format just
        consists of multiple binary versions concatenated. HOWEVER, the `revid` API only gives information
        about the first (current) version in the vector, except for the `asVersionVector` method. */
    class revid : public slice {
    public:
        revid()                                            :slice() {}
        revid(const void* b, size_t s)                     :slice(b,s) {}
        explicit revid(slice s)                            :slice(s) {}

        bool operator< (const revid&) const FLPURE;
        bool operator> (const revid &r) const FLPURE       {return r < *this;}

        /// Returns true if both revids represent the same revision:
        /// - If both are version vectors (or single versions) and their leading versions are equal
        /// - or if both are digest-based and are bitwise equal.
        bool isEquivalentTo(const revid&) const noexcept FLPURE;

        /// Returns true for version-vector style (gen@peer), false for rev-tree style (gen-digest).
        bool isVersion() const noexcept FLPURE             {return size > 0 && (*this)[0] == 0;}

        //---- Tree revision IDs only
        pair<unsigned,slice> generationAndDigest() const FLPURE;
        unsigned generation() const FLPURE;
        slice digest() const FLPURE                        {return generationAndDigest().second;}

        //---- Version IDs only
        Version asVersion() const FLPURE;
        VersionVector asVersionVector() const;

        //---- ASCII conversions:
        alloc_slice expanded() const;
        bool expandInto(slice_stream &dst) const noexcept;
        std::string str() const;
        explicit operator std::string() const       {return str();}
    };

    
    /** A self-contained revid that includes its own data buffer.

        PLEASE NOTE: the `parse` and `tryParse` methods can parse a single version, but not an entire
        VersionVector -- they will barf at the first comma. This is intentional. A `revidBuffer` is fixed-
        size and can't hold an arbitrarily long version vector. */
    class revidBuffer : public revid {
    public:
        revidBuffer()                               :revid(&_buffer, 0) {}
        revidBuffer(unsigned generation, slice digest);
        explicit revidBuffer(revid rev)             {*this = rev;}
        explicit revidBuffer(const Version &v)      {*this = v;}
        revidBuffer(const revidBuffer &r)           {*this = r;}

        /** Constructs a revidBuffer from an ASCII revision (digest or version style).
            Throws BadRevisionID if the string isn't parseable.*/
        explicit revidBuffer(slice asciiString)     :revid(&_buffer, 0) {parse(asciiString);}

        revidBuffer& operator= (const revidBuffer&) noexcept;
        revidBuffer& operator= (const revid&);
        revidBuffer& operator= (const Version &vers) noexcept;

        /** Parses a regular ASCII revID (digest or version style) and compresses it.
            Throws BadRevisionID if the string isn't parseable.
            \warning This will not parse an entire version vector, only its first component!
                    To parse the entire vector, call \ref VersionVector::fromASCII. */
        void parse(slice asciiString);

        /** Parses a regular ASCII revID (digest or version style) and compresses it.
            Returns false if the string isn't parseable.
            \warning This will not parse an entire version vector, only its first component!
                    To parse the entire vector, call \ref VersionVector::fromASCII. */
        bool tryParse(slice asciiString) noexcept;

    private:
        uint8_t _buffer[42];
    };
}
