//
// RevID.cc
//
// Copyright 2014-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "RevID.hh"
#include "VersionVector.hh"
#include "Error.hh"
#include "varint.hh"
#include "fleece/PlatformCompat.hh"
#include "StringUtil.hh"
#include "slice_stream.hh"
#include <math.h>
#include <limits.h>

namespace litecore {

    using namespace fleece;

    static inline bool islowerxdigit(char c) { return isxdigit(c) && !isupper(c); }

#pragma mark - API:

    pair<unsigned, slice> revid::generationAndDigest() const {
        if ( isVersion() ) error::_throw(error::InvalidParameter);
        slice_istream digest = *this;
        if ( auto gen = digest.readUVarInt(); !gen || *gen == 0 || *gen > UINT_MAX )
            error::_throw(error::CorruptRevisionData, "revid digest error");
        else
            return {unsigned(*gen), digest};
    }

    unsigned revid::generation() const {
        if ( isVersion() ) return unsigned(asVersion().gen());  //FIX: Should Version.gen change to uint32?
        else
            return generationAndDigest().first;
    }

    Version revid::asVersion() const {
        if ( isVersion() ) return VersionVector::readCurrentVersionFromBinary(*this);
        else if ( size == 0 )
            error::_throw(error::CorruptRevisionData, "revid reading version error");  // buffer too short!
        else
            error::_throw(error::InvalidParameter);  // it's a digest, not a version
    }

    VersionVector revid::asVersionVector() const {
        if ( isVersion() ) return VersionVector::fromBinary(*this);
        else if ( size == 0 )
            error::_throw(error::CorruptRevisionData, "revid reading version vector error");  // buffer too short!
        else
            error::_throw(error::InvalidParameter);  // it's a digest, not a version
    }

    bool revid::operator<(const revid& other) const {
        if ( isVersion() ) {
            return asVersion() < other.asVersion();
        } else {
            auto [myGen, myDigest]       = generationAndDigest();
            auto [otherGen, otherDigest] = other.generationAndDigest();
            return (myGen != otherGen) ? myGen < otherGen : myDigest < otherDigest;
        }
    }

    bool revid::isEquivalentTo(const revid& other) const noexcept {
        if ( *this == other ) return true;
        else
            return isVersion() && other.isVersion() && asVersion() == other.asVersion();
    }

    bool revid::expandInto(slice_ostream& result) const noexcept {
        slice_ostream out = result.capture();
        if ( isVersion() ) {
            if ( !asVersion().writeASCII(out) ) return false;
        } else {
            auto [gen, digest] = generationAndDigest();
            if ( !out.writeDecimal(gen) || !out.writeByte('-') || !out.writeHex(digest) ) return false;
        }
        result = out;
        return true;
    }

    alloc_slice revid::expanded() const {
        if ( !buf ) return alloc_slice();
        if ( isVersion() ) {
            return asVersion().asASCII();
        } else {
            auto [gen, digest]         = generationAndDigest();
            size_t        expandedSize = 2 + size_t(::floor(::log10(gen))) + 2 * digest.size;
            alloc_slice   resultBuf(expandedSize);
            slice_ostream out(resultBuf);
            Assert(expandInto(out));
            resultBuf.shorten(out.bytesWritten());
            return resultBuf;
        }
    }

    std::string revid::str() const {
        alloc_slice exp = expanded();
        return std::string((char*)exp.buf, exp.size);
    }

#pragma mark - RevIDBuffer:

    revidBuffer::revidBuffer(unsigned generation, slice digest) : revid(&_buffer, 0) {
        uint8_t* dst = _buffer;
        dst += PutUVarInt(dst, generation);
        setSize(dst + digest.size - _buffer);
        if ( size > sizeof(_buffer) ) error::_throw(error::BadRevisionID);  // digest too long!
        memcpy(dst, digest.buf, digest.size);
    }

    revidBuffer& revidBuffer::operator=(const revidBuffer& other) noexcept {
        memcpy(_buffer, other._buffer, sizeof(_buffer));
        set(&_buffer, other.size);
        return *this;
    }

    revidBuffer& revidBuffer::operator=(const revid& other) {
        if ( other.isVersion() ) {
            // Just copy the first Version:
            *this = other.asVersion();
        } else {
            if ( other.size > sizeof(_buffer) ) error::_throw(error::BadRevisionID);  // digest too long!
            memcpy(_buffer, other.buf, other.size);
            set(&_buffer, other.size);
        }
        return *this;
    }

    revidBuffer& revidBuffer::operator=(const Version& vers) noexcept {
        slice_ostream out(_buffer, sizeof(_buffer));
        out.writeByte(0);
        vers.writeBinary(out);
        *(slice*)this = out.output();
        return *this;
    }

    void revidBuffer::parse(slice s) {
        if ( !tryParse(s) ) error::_throw(error::BadRevisionID);
    }

    bool revidBuffer::tryParse(slice asciiData) noexcept {
        slice_istream ascii(asciiData);
        if ( ascii.findByte('-') != nullptr ) {
            // Digest type:
            uint8_t *start = _buffer, *end = start + sizeof(_buffer), *dst = start;
            set(start, 0);

            uint64_t gen = ascii.readDecimal();
            if ( gen == 0 || gen > UINT_MAX ) return false;
            dst += PutUVarInt(dst, gen);

            if ( ascii.readByte() != '-' ) return false;

            // Copy hex digest into dst as binary:
            if ( ascii.size == 0 || (ascii.size & 1) || dst + ascii.size / 2 > end )
                return false;  // rev ID is too long to fit in my buffer
            for ( unsigned i = 0; i < ascii.size; i += 2 ) {
                if ( !islowerxdigit(ascii[i]) || !islowerxdigit(ascii[i + 1]) ) return false;  // digest is not hex
                *dst++ = (uint8_t)(16 * digittoint(ascii[i]) + digittoint(ascii[i + 1]));
            }

            setEnd(dst);
            return true;
        } else {
            // Vector type:
            auto comma = ascii.findByteOrEnd(',');
            auto vers  = Version::readASCII(slice(ascii.buf, comma));
            if ( !vers ) return false;
            *this = *vers;
            return true;
        }
    }

}  // namespace litecore
