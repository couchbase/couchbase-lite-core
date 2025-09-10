//
// Version.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Version.hh"
#include "Base64.hh"
#include "Endian.hh"
#include "Error.hh"
#include "HybridClock.hh"
#include "RevID.hh"
#include "StringUtil.hh"
#include "slice_stream.hh"
#include <algorithm>

namespace litecore {
    using namespace std;
    using namespace fleece;

    Version Version::legacyVersion(revid oldRev) {
        // From "Migrating RevIDs to Version Vectors"
        // <https://docs.google.com/document/d/1hqeuQ7dlCD1XWsjnxRW6L5E15diLXkfo0Xn5F9vs13c>
        // - The generation becomes the upper 24 bits of the clock
        // - The first 40 bits of the digest become the lower 40 bits of the clock
        // - The legacy sourceID is kLegacyRevSourceID, which in ASCII is "Revision+Tree+Encoding".
        slice    digest = oldRev.digest();
        uint64_t n      = 0;
        ::memcpy(&n, digest.buf, std::min(size_t(8), digest.size));
        n = (fleece::endian::dec64(n) >> 24) | (uint64_t(oldRev.generation()) << 40);
        return Version(logicalTime(n), kLegacyRevSourceID);
    };

    static_assert(SourceID::kASCIILength == (sizeof(SourceID) * 4 + 2) / 3);

    string SourceID::asASCII() const {
        string str = base64::encode({&_bytes, sizeof(_bytes)});
        // Base64 encoding will always have a `==` suffix, so remove it:
        DebugAssert(str.size() == kASCIILength + 2);
        DebugAssert(hasSuffix(str, "=="));
        str.resize(kASCIILength);
        return str;
    }

    bool SourceID::writeASCII(fleece::slice_ostream& out) const { return out.write(asASCII()); }

    bool SourceID::readASCII(fleece::slice s) {
        if ( s.size != kASCIILength ) return false;

        // Append the `==` suffix required by the base64 decoder:
        char input[kASCIILength + 2];
        s.copyTo(input);
        input[kASCIILength] = input[kASCIILength + 1] = '=';

        // Now decode. The decoder requires a buffer of size 18, though the result only occupies
        // the first 16 bytes. If the other 2 bytes are nonzero, that means the final character
        // of the input wasn't valid; checking for this prevents multiple base64 strings from
        // decoding to the same binary SourceID, which could cause trouble.
        char  output[18] = {0};
        slice result     = base64::decode(slice(input, sizeof(input)), output, sizeof(output));
        if ( result.size != sizeof(SourceID) || output[16] != 0 || output[17] != 0 ) return false;
        result.copyTo(&_bytes);
        return true;
    }

    /*  BINARY PEERID ENCODING:
        First byte is the length of the following data: 0 or 16.
        - Length 0 denotes this is "me"; nothing follows.
        - Length 16 is a regular peer ID; the raw bytes follow. */

    bool SourceID::writeBinary(fleece::slice_ostream& out, bool current) const {
        uint8_t flag = current ? 0x80 : 0x00;
        if ( isMe() ) return out.writeByte(0 | flag);
        else
            return out.writeByte(sizeof(_bytes) | flag) && out.write(&_bytes, sizeof(_bytes));
    }

    bool SourceID::readBinary(fleece::slice_istream& in, bool* current) {
        uint8_t len = in.readByte();
        *current    = (len & 0x80) != 0;
        len &= 0x7F;
        if ( len == 0 ) {
            *this = kMeSourceID;
            return true;
        } else {
            return len == sizeof(_bytes) && in.readAll(&_bytes, len);
        }
    }

#pragma mark - VERSION:

    /*  BINARY HYBRIDTIME ENCODING:

        WHEREAS the lowest 16 bits of a logicalTime are a counter that's only used to break ties
            between equal time values; and
        WHEREAS that counter is usually zero;
        THEREFORE let the binary encoding add a LSB that's 1 when the counter's nonzero, and 0
            when the counter is zero. In the latter case the 16 bits of the counter are omitted.
    */

    static uint64_t compress(logicalTime g) {
        auto i = uint64_t(g);
        if ( i & 0xFFFF ) return (i << 1) | 1;
        else
            return i >> 15;
    }

    static logicalTime decompress(uint64_t i) {
        if ( i & 1 ) i >>= 1;  // If LSB is set, just remove it
        else
            i <<= 15;  // else add 15 more 0 bits
        return logicalTime(i);
    }

    Version::Version(slice ascii, SourceID mySourceID) {
        if ( !_readASCII(ascii) ) throwBadASCII(ascii);
        if ( _author == mySourceID ) _author = kMeSourceID;  // Abbreviate my ID
    }

    Version::Version(slice_istream& in) {
        optional<uint64_t> time = in.readUVarInt();
        if ( !time ) throwBadBinary();
        _time = decompress(*time);
        bool current;
        if ( !_author.readBinary(in, &current) ) throwBadBinary();
        validate();
    }

    /*static*/ optional<Version> Version::readASCII(slice ascii, SourceID mySourceID) {
        Version vers;
        if ( !vers._readASCII(ascii) ) return nullopt;
        if ( vers._author == mySourceID ) vers._author = kMeSourceID;  // Abbreviate my ID
        return vers;
    }

    bool Version::_readASCII(slice ascii) noexcept {
        slice_istream in = ascii;
        _time            = logicalTime{in.readHex()};
        if ( in.readByte() != '@' || _time == logicalTime::none ) return false;
        if ( in.peekByte() == '*' ) {
            in.readByte();
            _author = kMeSourceID;
        } else {
            if ( !_author.readASCII(in.readAll(SourceID::kASCIILength)) ) return false;
            if ( _author.isMe() ) return false;
        }
        return (in.size == 0);
    }

    void Version::validate() const {
        if ( _time == logicalTime::none ) error::_throw(error::BadRevisionID);
    }

    bool Version::writeBinary(slice_ostream& out, SourceID myID) const {
        SourceID const& id = _author.isMe() ? myID : _author;
        return out.writeUVarInt(compress(_time)) && id.writeBinary(out, false);
    }

    bool Version::writeASCII(slice_ostream& out, SourceID myID) const {
        if ( !out.writeHex(uint64_t(_time)) || !out.writeByte('@') ) return false;
        else if ( auto& author = (_author.isMe()) ? myID : _author; author.isMe() )
            return out.writeByte('*');
        else
            return author.writeASCII(out);
    }

    alloc_slice Version::asASCII(SourceID myID) const {
        auto result =
                slice_ostream::alloced(kMaxASCIILength, [&](slice_ostream& out) { return writeASCII(out, myID); });
        Assert(result);
        return result;
    }

    versionOrder Version::compare(logicalTime a, logicalTime b) {
        if ( a > b ) return kNewer;
        else if ( a < b )
            return kOlder;
        return kSame;
    }

    bool Version::updateClock(HybridClock& clock, bool anyone) const {
        return (!anyone && _author.isMe()) || clock.see(_time);
    }

    void Version::throwBadBinary() { error::_throw(error::BadRevisionID, "Invalid binary version ID"); }

    void Version::throwBadASCII(slice string) {
        if ( string ) error::_throw(error::BadRevisionID, "Invalid version string '%.*s'", SPLAT(string));
        else
            error::_throw(error::BadRevisionID, "Invalid version string");
    }


}  // namespace litecore
