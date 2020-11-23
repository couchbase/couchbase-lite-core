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
#include "VersionVector.hh"
#include "Error.hh"
#include "varint.hh"
#include "PlatformCompat.hh"
#include "StringUtil.hh"
#include <math.h>


namespace litecore {

    using namespace fleece;

    static constexpr const char kHexDigits[] = "0123456789abcdef";

    static inline bool islowerxdigit(char c) {
        return isxdigit(c) && !isupper(c);
    }


#pragma mark - API:


    std::pair<unsigned,slice> revid::generationAndDigest() const {
        if (isVersion())
            error::_throw(error::InvalidParameter);
        slice digest = *this;
        uint64_t gen;
        if (!ReadUVarInt(&digest, &gen) || gen == 0 || gen > UINT_MAX)
            error::_throw(error::CorruptRevisionData); // buffer too short!
        return {unsigned(gen), digest};
    }


    Version revid::asVersion() const {
        if (!isVersion())
            error::_throw(error::InvalidParameter);
        return VersionVector::readCurrentVersionFromBinary(*this);
    }

    bool revid::operator< (const revid& other) const {
        if (isVersion()) {
            return asVersion() < other.asVersion();
        } else {
            auto [myGen, myDigest] = generationAndDigest();
            auto [otherGen, otherDigest] = other.generationAndDigest();
            return (myGen != otherGen) ? myGen < otherGen : myDigest < otherDigest;
        }
    }

    bool revid::expandInto(slice &result) const {
        slice out = result;
        if (isVersion()) {
            if (!asVersion().writeASCII(&out))
                return false;
        } else {
            auto [gen, digest] = generationAndDigest();
            if (!out.writeDecimal(gen) || !out.writeByte('-') || out.size < 2 * digest.size)
                return false;
            auto dst = (char*)out.buf;
            for (size_t i = 0; i < digest.size; ++i) {
                *dst++ = kHexDigits[digest[i] >> 4];
                *dst++ = kHexDigits[digest[i] & 0x0F];
            }
            out.moveStart(2*digest.size);
        }
        result.shorten(result.size - out.size);
        return true;
    }

    alloc_slice revid::expanded() const {
        if (!buf)
            return alloc_slice();
        if (isVersion()) {
            return asVersion().asASCII();
        } else {
            auto [gen, digest] = generationAndDigest();
            size_t expandedSize = 2 + size_t(::floor(::log10(gen))) + 2*digest.size;
            alloc_slice resultBuf(expandedSize);
            slice result(resultBuf);
            Assert(expandInto(result));
            resultBuf.shorten(result.size);
            return resultBuf;
        }
    }

    std::string revid::str() const {
        alloc_slice exp = expanded();
        return std::string((char*)exp.buf, exp.size);
    }


#pragma mark - RevIDBuffer:


    revidBuffer::revidBuffer(unsigned generation, slice digest)
    :revid(&_buffer, 0)
    {
        uint8_t* dst = _buffer;
        dst += PutUVarInt(dst, generation);
        setSize(dst + digest.size - _buffer);
        if (size > sizeof(_buffer))
            error::_throw(error::BadRevisionID); // digest too long!
        memcpy(dst, digest.buf, digest.size);
    }


    revidBuffer& revidBuffer::operator= (const revidBuffer& other) {
        memcpy(_buffer, other._buffer, sizeof(_buffer));
        set(&_buffer, other.size);
        return *this;
    }


    revidBuffer& revidBuffer::operator= (const revid &other) {
        if (other.isVersion()) {
            // Just copy the first Version:
            *this = other.asVersion();
        } else {
            if (other.size > sizeof(_buffer))
                error::_throw(error::BadRevisionID); // digest too long!
            memcpy(_buffer, other.buf, other.size);
            set(&_buffer, other.size);
        }
        return *this;
    }


    revidBuffer& revidBuffer::operator= (const Version &vers) {
        slice out(_buffer, sizeof(_buffer));
        out.writeByte(0);
        vers.writeBinary(&out);
        set(&_buffer, 0);
        setEnd(out.buf);
        return *this;
    }


    void revidBuffer::parse(slice s) {
        if (!tryParse(s))
            error::_throw(error::BadRevisionID);
    }

    bool revidBuffer::tryParse(slice ascii) {
        uint8_t* start = _buffer, *end = start + sizeof(_buffer), *dst = start;
        set(start, 0);

        uint64_t gen = ascii.readDecimal();
        if (gen == 0 || gen > UINT_MAX || ascii.size == 0)
            return false;

        if (ascii[0] != '-')
            return false;
        ascii.moveStart(1);

        dst += PutUVarInt(dst, gen);

        // Copy hex digest into dst as binary:
        if (ascii.size == 0 || (ascii.size & 1) || dst + ascii.size / 2 > end)
            return false; // rev ID is too long to fit in my buffer
        for (unsigned i = 0; i < ascii.size; i += 2) {
            if (!islowerxdigit(ascii[i]) || !islowerxdigit(ascii[i+1]))
                return false; // digest is not hex
            *dst++ = (uint8_t)(16*digittoint(ascii[i]) + digittoint(ascii[i+1]));
        }
        setEnd(dst);
        return true;
    }

}
