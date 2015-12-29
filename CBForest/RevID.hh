//
//  RevID.hh
//  CBForest
//
//  Created by Jens Alfke on 6/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef __CBForest__RevID__
#define __CBForest__RevID__

#include "slice.hh"

namespace cbforest {

    /** A compressed revision ID. 
        Since this is based on slice, it doesn't own the memory it points to. */
    class revid : public slice {
    public:
        revid()                                     :slice() {}
        revid(const void* b, size_t s)              :slice(b,s) {}
        explicit revid(slice s)                     :slice(s) {}

        bool isCompressed() const                   {return !isdigit((*this)[0]);}

        alloc_slice expanded() const;
        size_t expandedSize() const;
        bool expandInto(slice &dst) const;

        unsigned generation() const;
        slice digest() const;
        bool operator< (const revid&) const;

        explicit operator std::string() const;
#ifdef __OBJC__
        explicit operator NSString*() const; // overrides slice method
#endif

    private:
        uint64_t getGenAndDigest(slice &digest) const;
        void _expandInto(slice &dst) const;
    };

    /** A self-contained revid that includes its own data buffer. */
    class revidBuffer : public revid {
    public:
        revidBuffer()                               :revid(&_buffer, 0) {}
        explicit revidBuffer(slice s)               :revid(&_buffer, 0) {parse(s);}
        revidBuffer(unsigned generation, slice digest);
        revidBuffer(const revidBuffer&);

        /** Parses a regular (uncompressed) revID and compresses it.
            Throws BadRevisionID if the revID isn't in the proper format.*/
        void parse(slice);
        
#ifdef __OBJC__
        explicit revidBuffer(NSString* str);
#endif

    private:
        uint8_t _buffer[42];
    };
}

#endif /* defined(__CBForest__RevID__) */
