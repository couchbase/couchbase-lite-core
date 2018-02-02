//
// RevID.cc
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

#include "RevID.hh"
#include "Error.hh"
#include "varint.hh"
#include "PlatformCompat.hh"
#include <math.h>


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


namespace litecore {

    using namespace fleece;

    // Parses bytes from str to end as a decimal ASCII number. Returns 0 if non-digit found.
    static inline uint64_t parseDigits(const char *str, const char *end)
    {
        uint64_t result = 0;
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


    slice revid::skipFlag() const {
        slice skipped = *this;
        if (skipped.size > 0 && skipped[0] == 0)
            skipped.moveStart(1);
        return skipped;
    }


    uint64_t revid::getGenAndDigest(slice &digest) const {
        digest = skipFlag();
        uint64_t gen;
        if (!ReadUVarInt(&digest, &gen))
            error::_throw(error::CorruptRevisionData); // buffer too short!
        return gen;
    }


    size_t revid::expandedSize() const {
        slice digest;
        uint64_t gen = getGenAndDigest(digest);
        size_t size = 2 + size_t(::floor(::log10(gen)));    // digits and separator
        if (isClock())
            size += digest.size;
        else
            size += 2*digest.size;
        return size;
    }

    void revid::_expandInto(slice &expanded_rev) const {
        slice digest;
        uint64_t gen = getGenAndDigest(digest);

        char* dst = (char*)expanded_rev.buf;
        dst += writeDigits(dst, gen);

        if (isClock()) {
            *dst++ = '@';
            memcpy(dst, digest.buf, digest.size);
            dst += digest.size;
        } else {
            *dst++ = '-';
            const uint8_t* bytes = (const uint8_t*)digest.buf;
            for (size_t i = 0; i < digest.size; ++i)
                dst = byteToHex(dst, bytes[i]);
        }
        expanded_rev.setSize(dst - (char*)expanded_rev.buf);
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
        alloc_slice resultBuf(expandedSize());
        slice result(resultBuf);
        _expandInto(result);
        resultBuf.shorten(result.size);
        return resultBuf;
    }

    unsigned revid::generation() const {
        uint64_t gen;
        if (GetUVarInt(skipFlag(), &gen) == 0)
            error::_throw(error::CorruptRevisionData); // buffer too short!
        return (unsigned) gen;
    }

    slice revid::digest() const {
        uint64_t gen;
        slice digest = skipFlag();
        if (!ReadUVarInt(&digest, &gen))
            error::_throw(error::CorruptRevisionData); // buffer too short!
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


#pragma mark - RevIDBuffer:


    revidBuffer::revidBuffer(const revidBuffer& other) {
        *this = other;
    }

    revidBuffer& revidBuffer::operator= (const revidBuffer& other) {
        memcpy(_buffer, other._buffer, sizeof(_buffer));
        set(&_buffer, other.size);
        return *this;
    }

    revidBuffer& revidBuffer::operator= (const revid &other) {
        Assert(other.size <= sizeof(_buffer));
        memcpy(_buffer, other.buf, other.size);
        set(&_buffer, other.size);
        return *this;
    }


    revidBuffer::revidBuffer(unsigned generation, slice digest, revidType type)
    :revid(&_buffer, 0)
    {
        uint8_t* dst = _buffer;
        if (type == kClockType)
            *(dst++) = 0;
        dst += PutUVarInt(dst, generation);
        setSize(dst + digest.size - _buffer);
        if (size > sizeof(_buffer))
            error::_throw(error::CorruptRevisionData); // digest too long!
        memcpy(dst, digest.buf, digest.size);
    }


    void revidBuffer::parse(slice s, bool allowClock) {
        if (!tryParse(s, allowClock))
            error::_throw(error::BadRevisionID);
    }

    void revidBuffer::parseNew(slice s) {
        if (!tryParse(s, true))
            error::_throw(error::BadRevisionID);
    }

    bool revidBuffer::tryParse(slice ascii, bool allowClock) {
        uint8_t* start = _buffer, *dst = start;
        set(start, 0);

        // Find the separator; if it's '-' this is a digest type, if it's '@' it's a clock:
        const char *sep = (const char*)ascii.findByte('@');
        bool isClock = (sep != nullptr);
        if (isClock) {
            if (!allowClock)
                return false;
            *dst++ = 0; // leading zero byte denotes clock-style revid
        } else {
            sep = (const char*)ascii.findByte('-');
            if (sep == nullptr)
                return false; // separator is missing
        }

        ssize_t sepPos = sep - (const char*)ascii.buf;
        if (sepPos == 0 || sepPos > 20 || sepPos >= ascii.size-1)
            return false; // generation too large, or separator at end

        uint64_t gen = parseDigits((const char*)ascii.buf, sep);
        if (gen == 0)
            return false; // unparseable generation
        size_t genSize = PutUVarInt(dst, gen);
        dst += genSize;

        slice suffix = ascii;
        suffix.moveStart(sepPos + 1);

        if (isClock) {
            if (1 + genSize + suffix.size > sizeof(_buffer))
                return false; // rev ID is too long to fit in my buffer
            memcpy(dst, suffix.buf, suffix.size);
            dst += suffix.size;
        } else {
            if (genSize + suffix.size/2 > sizeof(_buffer) || (suffix.size & 1))
                return false; // rev ID is too long to fit in my buffer
            for (unsigned i=0; i<suffix.size; i+=2) {
                if (!isxdigit(suffix[i]) || !isxdigit(suffix[i+1]))
                    return false; // digest is not hex
                *dst++ = (uint8_t)(16*digittoint(suffix[i]) + digittoint(suffix[i+1]));
            }
        }
        setSize(dst - start);
        return true;
    }

}
