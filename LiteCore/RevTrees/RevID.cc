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
#include "slice_stream.hh"
#ifndef __APPLE__
#    include "StringUtil.hh"
#endif
#include <cmath>
#include <climits>

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
        if ( isVersion() ) error::_throw(error::InvalidParameter, "version revids have no generations");
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
            return Version::byAscendingTimes(asVersion(), other.asVersion());
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

    bool revid::expandInto(slice_ostream& dst) const noexcept {
        slice_ostream out = dst.capture();
        if ( isVersion() ) {
            if ( !asVersion().writeASCII(out) ) return false;
        } else {
            auto [gen, digest] = generationAndDigest();
            if ( !out.writeDecimal(gen) || !out.writeByte('-') || !out.writeHex(digest) ) return false;
        }
        dst = out;
        return true;
    }

    alloc_slice revid::expanded() const {
        if ( !buf ) return {};
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
        return {(char*)exp.buf, exp.size};
    }

#pragma mark - RevIDBuffer:

    revidBuffer::revidBuffer(unsigned generation, slice digest) : _revid(&_buffer, 0) {
        uint8_t* dst = _buffer;
        dst += PutUVarInt(dst, generation);
        _revid.setSize(dst + digest.size - _buffer);
        if ( _revid.size > sizeof(_buffer) ) error::_throw(error::BadRevisionID);  // digest too long!
        memcpy(dst, digest.buf, digest.size);
    }

    revidBuffer& revidBuffer::operator=(const revidBuffer& other) noexcept {
        if ( &other == this ) return *this;
        memcpy(_buffer, other._buffer, sizeof(_buffer));
        _revid.set(&_buffer, other._revid.size);
        return *this;
    }

    revidBuffer& revidBuffer::operator=(const revid& other) {
        if ( other.isVersion() ) {
            // Just copy the first Version:
            *this = other.asVersion();
        } else {
            if ( other.size > sizeof(_buffer) ) error::_throw(error::BadRevisionID);  // digest too long!
            memcpy(_buffer, other.buf, other.size);
            _revid.set(&_buffer, other.size);
        }
        return *this;
    }

    revidBuffer& revidBuffer::operator=(const Version& vers) noexcept {
        slice_ostream out(_buffer, sizeof(_buffer));
        out.writeByte(0);  // flag indicating this is a binary VV
        Assert(vers.writeBinary(out));
        *(slice*)&_revid = out.output();
        return *this;
    }

    void revidBuffer::parse(slice asciiString) {
        if ( !tryParse(asciiString) ) error::_throw(error::BadRevisionID);
    }

    bool revidBuffer::tryParse(slice asciiString) noexcept {
        slice_istream ascii(asciiString);
        if ( ascii.findByte('-') != nullptr ) {
            // Digest type:
            uint8_t *start = _buffer, *end = start + sizeof(_buffer), *dst = start;
            _revid.set(start, 0);

            uint64_t gen = ascii.readDecimal();
            if ( gen == 0 || gen > UINT_MAX ) return false;
            dst += PutUVarInt(dst, gen);

            if ( ascii.readByte() != '-' ) return false;

            // Copy hex digest into dst as binary:
            if ( ascii.size == 0 || (ascii.size & 1) || dst + ascii.size / 2 > end )
                return false;  // rev ID is too long to fit in my buffer
            for ( unsigned i = 0; i < ascii.size; i += 2 ) {
                if ( !islowerxdigit(static_cast<char>(ascii[i])) || !islowerxdigit(static_cast<char>(ascii[i + 1])) )
                    return false;  // digest is not hex
                *dst++ = (uint8_t)(16 * digittoint(ascii[i]) + digittoint(ascii[i + 1]));
            }

            _revid.setEnd(dst);
            return true;
        } else {
            // Vector type:
            auto vers = VersionVector::readCurrentVersionFromASCII(ascii);
            if ( !vers ) return false;
            *this = *vers;
            return true;
        }
    }

}  // namespace litecore
