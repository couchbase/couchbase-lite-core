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
        The data format is the generation as a varint, followed by the digest as raw binary. */
    class revid : public slice {
    public:
        revid()                                     :slice() {}
        revid(const void* b, size_t s)              :slice(b,s) {}
        explicit revid(slice s)                     :slice(s) {}

        std::pair<unsigned,slice> generationAndDigest() const;
        unsigned generation() const                 {return generationAndDigest().first;}
        slice digest() const                        {return generationAndDigest().second;}

        bool isVersion() const                      {return (*this)[0] == 0;}
        Version asVersion() const;
        VersionVector asVersionVector() const;

        bool operator< (const revid&) const;
        bool operator> (const revid &r) const       {return r < *this;}

        //---- ASCII conversions:
        alloc_slice expanded() const;
        bool expandInto(slice &dst) const;
        std::string str() const;
        explicit operator std::string() const       {return str();}
    };

    /** A self-contained revid that includes its own data buffer. */
    class revidBuffer : public revid {
    public:
        revidBuffer()                               :revid(&_buffer, 0) {}
        revidBuffer(unsigned generation, slice digest);
        explicit revidBuffer(revid rev)             {*this = rev;}
        explicit revidBuffer(const Version &v)      {*this = v;}
        revidBuffer(const revidBuffer &r)           {*this = r;}

        explicit revidBuffer(slice s)               :revid(&_buffer, 0) {parse(s);}

        revidBuffer& operator= (const revidBuffer&);
        revidBuffer& operator= (const revid&);
        revidBuffer& operator= (const Version &vers);

        /** Parses a regular (uncompressed) revID and compresses it.
            Throws BadRevisionID if the revID isn't in the proper format.*/
        void parse(slice);

        bool tryParse(slice ascii);

    private:
        uint8_t _buffer[42];
    };
}
