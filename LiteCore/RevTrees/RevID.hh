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

    enum revidType {
        kDigestType,    // Old type, generation + digest: "7-deadbeef"
        kClockType      // New type, generation + source ID: "7@1234"
    };


    using SourceID = uint64_t;


    /** A compressed revision ID. 
        Since this is based on slice, it doesn't own the memory it points to.
        The data format is the generation as a varint, followed by the digest as raw binary. */
    class revid : public slice {
    public:
        revid()                                     :slice() {}
        revid(const void* b, size_t s)              :slice(b,s) {}
        explicit revid(slice s)                     :slice(s) {}

        alloc_slice expanded() const;
        size_t expandedSize() const;
        bool expandInto(slice &dst) const;

        bool isClock() const                        {return (*this)[0] == 0;}

        std::pair<unsigned,slice> generationAndDigest() const;
        unsigned generation() const                 {return generationAndRest().first;}
        slice digest() const                        {return generationAndDigest().second;}

        std::pair<unsigned,SourceID> generationAndSource() const;
        SourceID source() const                     {return generationAndSource().second;}

        bool operator< (const revid&) const;
        bool operator> (const revid &r) const       {return r < *this;}

        explicit operator std::string() const;

    private:
        slice skipFlag() const;
        std::pair<unsigned,slice> generationAndRest() const;
        void _expandInto(slice &dst) const;
    };

    /** A self-contained revid that includes its own data buffer. */
    class revidBuffer : public revid {
    public:
        revidBuffer()                               :revid(&_buffer, 0) {}
        revidBuffer(revid rev)                      :revid(&_buffer, rev.size)
        {memcpy(&_buffer, rev.buf, rev.size);}

        explicit revidBuffer(slice s, bool allowClock =false)
        :revid(&_buffer, 0)
        {parse(s, allowClock);}

        revidBuffer(unsigned generation, slice digest, revidType);
        revidBuffer(const revidBuffer&);
        revidBuffer& operator= (const revidBuffer&);
        revidBuffer& operator= (const revid&);

        /** Parses a regular (uncompressed) revID and compresses it.
            Throws BadRevisionID if the revID isn't in the proper format.*/
        void parse(slice, bool allowClock =false);

        void parseNew(slice s);

        bool tryParse(slice ascii, bool allowClock =false);

    private:
        uint8_t _buffer[42];
    };
}
