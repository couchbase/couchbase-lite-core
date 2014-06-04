//
//  RevID.cc
//  CBForest
//
//  Created by Jens Alfke on 6/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "RevID.hh"
#include "varint.h"
#include <assert.h>


namespace forestdb {

    // Parses bytes from str to end as an ASCII number. Returns 0 if non-digit found.
    static uint32_t parseDigits(const char *str, const char *end)
    {
        uint32_t result = 0;
        for (; str < end; ++str) {
            if (!isdigit(*str))
                return 0;
            result = 10*result + (*str - '0');
        }
        return result;
    }


    static bool ParseUncompacted(slice rev, unsigned *generation, slice *digest) {
        const char *dash = (const char*)::memchr(rev.buf, '-', rev.size);
        if (dash == NULL || dash == rev.buf) {
            *generation = 0;
            return false;
        }
        ssize_t dashPos = dash - (const char*)rev.buf;
        if (dashPos > 8 || dashPos >= rev.size - 1) {
            *generation = 0;
            return false;
        }
        *generation = parseDigits((const char*)rev.buf, dash);
        if (*generation == 0) {
            return false;
        }
        if (digest) {
            digest->buf = (char*)dash + 1;
            digest->size = (uint8_t*)rev.buf + rev.size - (uint8_t*)digest->buf;
        }
        return true;
    }


    static bool ParseCompacted(slice rev, unsigned *generation, slice *digest) {
        unsigned gen = ((uint8_t*)rev.buf)[0];
        if (isdigit(gen))
            return ParseUncompacted(rev, generation, digest);
        if (gen > '9')
            gen -= 10;
        *generation = gen;
        if (digest)
            *digest = (slice){(uint8_t*)rev.buf + 1, rev.size - 1};
        return true;
    }


    static void copyBuf(slice srcrev, slice *dstrev) {
        dstrev->size = srcrev.size;
        memcpy((void*)dstrev->buf, srcrev.buf, srcrev.size);
    }

    static inline char* byteToHex(char *dst, uint8_t byte) {
        static const char hexchar[] = "0123456789abcdef";
        dst[0] = hexchar[byte >> 4];
        dst[1] = hexchar[byte & 0x0F];
        return dst + 2;
    }


    static bool Compact(slice srcrev, slice *dstrev) {
        unsigned generation;
        slice digest;
        if (!ParseUncompacted(srcrev, &generation, &digest))
            return false;
        else if (generation > 245 || (digest.size & 1)) {
            copyBuf(srcrev, dstrev);
            return true;
        }
        const char* src = (const char*)digest.buf;
        for (unsigned i=0; i<digest.size; i++) {
            if (!isxdigit(src[i])) {
                copyBuf(srcrev, dstrev);
                return true;
            }
        }

        uint8_t encodedGen = (uint8_t)generation;
        if (generation >= '0')
            encodedGen += 10; // skip digit range
        char* buf = (char*)dstrev->buf, *dst = buf;
        *dst++ = encodedGen;
        for (unsigned i=0; i<digest.size; i+=2)
            *dst++ = (char)(16*digittoint(src[i]) + digittoint(src[i+1]));
        dstrev->size = dst - buf;
        return true;
    }


    static size_t ExpandedSize(slice rev) {
        unsigned generation = ((const uint8_t*)rev.buf)[0];
        if (isdigit(generation))
            return 0; // uncompressed
        if (generation > '9')
            generation -= 10;
        return 2 + (generation >= 10) + (generation >= 100) + 2*(rev.size-1);
    }


    static void Expand(slice rev, slice* expanded_rev) {
        const uint8_t* src = (const uint8_t*)rev.buf;
        if (isdigit(src[0])) {
            expanded_rev->size = rev.size;
            memcpy((void*)expanded_rev->buf, rev.buf, rev.size);
            return;
        }
        unsigned generation = src[0];
        if (generation > '9')
            generation -= 10;
        char *buf = (char*)expanded_rev->buf, *dst = buf;
        dst += sprintf(dst, "%u-", generation);
        for (unsigned i=1; i<rev.size; i++)
            dst = byteToHex(dst, src[i]);
        expanded_rev->size = dst - buf;
    }


#pragma mark - API:


    alloc_slice revid::expanded() const {
        size_t size = ExpandedSize(*this);
        if (size == 0)
            return alloc_slice(*this); // I'm already expanded
        alloc_slice result(size);
        bool expanded = expandInto(result);
        assert(expanded);
        return result;
    }

    bool revid::expandInto(forestdb::slice &dst) const {
        if (dst.size < ExpandedSize(*this))
            return false;
        Expand(*this, &dst);
        return true;
    }

    size_t revid::expandedSize() const {
        return ExpandedSize(*this);
    }

    unsigned revid::generation() const {
        uint64_t gen;
        GetUVarInt(*(::slice*)this, &gen);
        return (unsigned) gen;
    }

    bool revidBuffer::parse(slice raw) {
        unsigned gen;
        slice hexDigest;
        size = 0;
        if (!ParseUncompacted(raw, &gen, &hexDigest) || (hexDigest.size & 1))
            return false;

        uint8_t* start = (uint8_t*) buf, *dst = start;
        size_t genSize = PutUVarInt(dst, gen);
        if (genSize + hexDigest.size/2 > sizeof(_buffer))
            return false;
        dst += genSize;
        for (unsigned i=0; i<hexDigest.size; i+=2) {
            if (!isxdigit(hexDigest[i]) || !isxdigit(hexDigest[i+1])) {
                return false;
            }
            *dst++ = (uint8_t)(16*digittoint(hexDigest[i]) + digittoint(hexDigest[i+1]));
        }
        size = dst - start;
        return true;
    }

}
