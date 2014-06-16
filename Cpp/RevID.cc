//
//  RevID.cc
//  CBForest
//
//  Created by Jens Alfke on 6/2/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#include "RevID.hh"
#include "Error.hh"
#include "varint.h"
#include <assert.h>
#include <math.h>


namespace forestdb {

    // Parses bytes from str to end as a decimal ASCII number. Returns 0 if non-digit found.
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

    // Writes a byte to dst as two hex digits.
    static inline char* byteToHex(char *dst, uint8_t byte) {
        static const char hexchar[] = "0123456789abcdef";
        dst[0] = hexchar[byte >> 4];
        dst[1] = hexchar[byte & 0x0F];
        return dst + 2;
    }


#pragma mark - API:


    size_t revid::expandedSize() const {
        slice buf = *this;
        uint64_t gen;
        ::ReadUVarInt((::slice*)&buf, &gen);
        size_t genDigits = 1 + size_t(::floor(::log10(gen)));
        return genDigits + 1 + 2*buf.size;
    }

    void revid::_expandInto(slice &expanded_rev) const {
        const uint8_t* src = (const uint8_t*)buf;
        unsigned generation = src[0];
        if (generation > '9')
            generation -= 10;
        char *buf = (char*)expanded_rev.buf, *dst = buf;
        dst += sprintf(dst, "%u-", generation);
        for (unsigned i=1; i<size; i++)
            dst = byteToHex(dst, src[i]);
        expanded_rev.size = dst - buf;
    }

    bool revid::expandInto(slice &expanded_rev) const {
        if (expanded_rev.size < expandedSize())
            return false;
        _expandInto(expanded_rev);
        return true;
    }

    alloc_slice revid::expanded() const {
        alloc_slice result(expandedSize());
        _expandInto(result);
        return result;
    }

    unsigned revid::generation() const {
        uint64_t gen;
        if (GetUVarInt(*(::slice*)this, &gen) == 0)
            throw error(error::CorruptRevisionData); // buffer too short!
        return (unsigned) gen;
    }

    slice revid::digest() const {
        uint64_t gen;
        slice digest = *this;
        ReadUVarInt((::slice*)&digest, &gen);
        return digest;
    }

    bool revid::operator< (const revid& other) const {
        unsigned myGen = generation(), otherGen = other.generation();
        if (myGen != otherGen)
            return myGen < otherGen;
        return digest() < other.digest();
    }


    revidBuffer::revidBuffer(const revidBuffer& other) {
        memcpy(_buffer, other._buffer, sizeof(_buffer));
        buf = &_buffer;
        size = other.size;
    }


    void revidBuffer::parse(slice raw) {
        size = 0;

        const char *dash = (const char*)::memchr(raw.buf, '-', raw.size);
        if (dash == NULL || dash == raw.buf)
            throw error(error::BadRevisionID); // '-' is missing or at start of string
        ssize_t dashPos = dash - (const char*)raw.buf;
        if (dashPos > 8 || dashPos >= raw.size - 1)
            throw error(error::BadRevisionID); // generation too large
        unsigned gen = parseDigits((const char*)raw.buf, dash);
        if (gen == 0)
            throw error(error::BadRevisionID); // unparseable generation

        uint8_t* start = (uint8_t*) buf, *dst = start;
        size_t genSize = PutUVarInt(dst, gen);

        slice hexDigest = raw;
        hexDigest.moveStart(dashPos + 1);

        if (genSize + hexDigest.size/2 > sizeof(_buffer))
            throw error(error::BadRevisionID); // rev ID is too long to fit in my buffer
        dst += genSize;
        for (unsigned i=0; i<hexDigest.size; i+=2) {
            if (!isxdigit(hexDigest[i]) || !isxdigit(hexDigest[i+1])) {
                throw error(error::BadRevisionID); // digest is not hex
            }
            *dst++ = (uint8_t)(16*digittoint(hexDigest[i]) + digittoint(hexDigest[i+1]));
        }
        size = dst - start;
    }

}
