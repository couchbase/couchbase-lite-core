//
//  RevID.cc
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

#include "RevID.hh"
#include "Error.hh"
#include "varint.hh"
#include <math.h>
#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif


#if defined(__ANDROID__) || defined(__GLIBC__) || defined(_MSC_VER)
// digittoint is a BSD function, not available on Android, Linux, etc.
static int digittoint(char ch) {
    int d = ch - '0';
    if ((unsigned) d < 10) {
        return d;
    }
    d = ch - 'a';
    if ((unsigned) d < 6) {
        return d + 10;
    }
    d = ch - 'A';
    if ((unsigned) d < 6) {
        return d + 10;
    }
    return 0;
}
#endif // defined(__ANDROID__) || defined(__GLIBC__)


namespace cbforest {

    // Parses bytes from str to end as a decimal ASCII number. Returns 0 if non-digit found.
    static inline uint32_t parseDigits(const char *str, const char *end)
    {
        uint32_t result = 0;
        for (; str < end; ++str) {
            if (!isdigit(*str))
                return 0;
            result = 10*result + (*str - '0');
        }
        return result;
    }

    // Writes the decimal representation of n to str, returning the number of digits written (1-20).
    static inline size_t writeDigits(char *str, uint64_t n) {
        size_t len;
        if (n < 10) {
            str[0] = '0' + (char)n;
            len = 1;
        } else {
            char temp[20]; // max length is 20 decimal digits
            char *dst = &temp[20];
            len = 0;
            do {
                *(--dst) = '0' + (n % 10);
                n /= 10;
                len++;
            } while (n > 0);
            memcpy(str, dst, len);
        }
        str[len] = '\0';
        return len;
    }
    
    // Writes a byte to dst as two hex digits.
    static inline char* byteToHex(char *dst, uint8_t byte) {
        static const char hexchar[] = "0123456789abcdef";
        dst[0] = hexchar[byte >> 4];
        dst[1] = hexchar[byte & 0x0F];
        return dst + 2;
    }


#pragma mark - API:


    uint64_t revid::getGenAndDigest(slice &digest) const {
        digest = *this;
        uint64_t gen;
        if (!ReadUVarInt(&digest, &gen))
            throw error(error::CorruptRevisionData); // buffer too short!
        return gen;
    }


    size_t revid::expandedSize() const {
        slice digest;
        uint64_t gen = getGenAndDigest(digest);
        size_t genDigits = 1 + size_t(::floor(::log10(gen)));
        return genDigits + 1 + 2*digest.size;
    }

    void revid::_expandInto(slice &expanded_rev) const {
        slice digest;
        uint64_t gen = getGenAndDigest(digest);

        char* dst = (char*)expanded_rev.buf;
        dst += writeDigits(dst, gen);
        *dst++ = '-';

        const uint8_t* bytes = (const uint8_t*)digest.buf;
        for (size_t i = 0; i < digest.size; ++i)
            dst = byteToHex(dst, bytes[i]);
        expanded_rev.size = dst - (char*)expanded_rev.buf;
    }

    bool revid::expandInto(slice &expanded_rev) const {
        if (expanded_rev.size < expandedSize())
            return false;
        _expandInto(expanded_rev);
        return true;
    }

    alloc_slice revid::expanded() const {
        if (!buf)
            return alloc_slice();
        alloc_slice result(expandedSize());
        _expandInto(result);
        return result;
    }

    unsigned revid::generation() const {
        uint64_t gen;
        if (GetUVarInt(*this, &gen) == 0)
            throw error(error::CorruptRevisionData); // buffer too short!
        return (unsigned) gen;
    }

    slice revid::digest() const {
        uint64_t gen;
        slice digest = *this;
        if (!ReadUVarInt(&digest, &gen))
            throw error(error::CorruptRevisionData); // buffer too short!
        return digest;
    }

    bool revid::operator< (const revid& other) const {
        unsigned myGen = generation(), otherGen = other.generation();
        if (myGen != otherGen)
            return myGen < otherGen;
        return digest() < other.digest();
    }

    revid::operator std::string() const {
        alloc_slice exp = expanded();
        return std::string((char*)exp.buf, exp.size);
    }



    revidBuffer::revidBuffer(const revidBuffer& other) {
        memcpy(_buffer, other._buffer, sizeof(_buffer));
        buf = &_buffer;
        size = other.size;
    }


    revidBuffer::revidBuffer(unsigned generation, slice digest)
    :revid(&_buffer, 0)
    {
        auto genSize = PutUVarInt((void*)buf, generation);
        size = genSize + digest.size;
        if (size > sizeof(_buffer))
            throw error(error::CorruptRevisionData); // digest too long!
        memcpy(&_buffer[genSize], digest.buf, digest.size);
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
