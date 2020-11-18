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
#include "StringUtil.hh"
#include <math.h>


namespace litecore {

    using namespace fleece;

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

    static inline bool islowerxdigit(char c) {
        return isxdigit(c) && !isupper(c);
    }


#pragma mark - API:


    slice revid::skipFlag() const {
        slice skipped = *this;
        if (skipped.size > 0 && skipped[0] == 0)
            skipped.moveStart(1);
        return skipped;
    }


    std::pair<unsigned,slice> revid::generationAndRest() const {
        slice digest = skipFlag();
        uint64_t gen;
        if (!ReadUVarInt(&digest, &gen) || gen == 0 || gen > UINT_MAX)
            error::_throw(error::CorruptRevisionData); // buffer too short!
        return {unsigned(gen), digest};
    }


    std::pair<unsigned,slice> revid::generationAndDigest() const {
        if (isClock())
            error::_throw(error::InvalidParameter);
        return generationAndRest();
    }


    std::pair<unsigned,SourceID> revid::generationAndSource() const {
        if (!isClock())
            error::_throw(error::InvalidParameter);
        auto [gen, buf] = generationAndDigest();
        SourceID source;
        if (!ReadUVarInt(&buf, &source) || buf.size > 0)
            error::_throw(error::CorruptRevisionData);
        return {gen, source};
    }

    size_t revid::expandedSize() const {
        auto [gen, digest] = generationAndDigest();
        size_t size = 2 + size_t(::floor(::log10(gen)));    // digits and separator
        if (isClock())
            size += digest.size;
        else
            size += 2*digest.size;
        return size;
    }

    void revid::_expandInto(slice &expanded_rev) const {
        auto [gen, digest] = generationAndDigest();

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
        uint8_t* start = _buffer, *end = start + sizeof(_buffer), *dst = start;
        set(start, 0);

        uint64_t gen = ascii.readDecimal();
        if (gen == 0 || gen > UINT_MAX || ascii.size == 0)
            return false;

        bool isClock = (ascii[0] == '@');
        if (isClock) {
            if (!allowClock)
                return false;
            *dst++ = 0; // leading zero byte denotes clock-style revid
        } else if (ascii[0] != '-') {
            return false;
        }
        ascii.moveStart(1);

        dst += PutUVarInt(dst, gen);

        if (isClock) {
            // Read source ID as hex number:
            if (ascii.size > 2 * sizeof(SourceID))
                return false;
            SourceID source = ascii.readHex();
            if (ascii.size > 0)
                return false;
            dst += PutUVarInt(dst, source);
        } else {
            // Copy hex digest into dst as binary:
            if (ascii.size == 0 || (ascii.size & 1) || dst + ascii.size / 2 > end)
                return false;
            for (unsigned i = 0; i < ascii.size; i += 2) {
                if (!islowerxdigit(ascii[i]) || !islowerxdigit(ascii[i+1]))
                    return false; // digest is not hex
                *dst++ = (uint8_t)(16*digittoint(ascii[i]) + digittoint(ascii[i+1]));
            }
        }
        setEnd(dst);
        return true;
    }

}
