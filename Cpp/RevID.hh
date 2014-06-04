//
//  RevID.h
//  CBForest
//
//  Created by Jens Alfke on 6/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef __CBForest__RevID__
#define __CBForest__RevID__

#include "slice.h"

namespace forestdb {

    /** A compressed revision ID. This class is immutable. */
    class revid : public slice {
    public:
        revid()                                     :slice() {}
        revid(const void* b, size_t s)              :slice(b,s) {}

        revid& operator= (const slice& s)           {buf=s.buf; size=s.size; return *this;}

        bool isCompressed() const                   {return !isdigit((*this)[0]);}

        alloc_slice expanded() const;
        size_t expandedSize() const;
        bool expandInto(slice &dst) const;

        unsigned generation() const;
    };

    /** A self-contained revid that includes its own data buffer. */
    class revidBuffer : public revid {
    public:
        /** Parses a regular (uncompressed) revID and compresses it. */
        bool parse(slice);
        
    private:
        revidBuffer()                               :revid(&_buffer, 0) {}
        uint8_t _buffer[42];
    };
}

#endif /* defined(__CBForest__RevID__) */
