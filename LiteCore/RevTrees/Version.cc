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
#include "Error.hh"
#include "StringUtil.hh"
#include "varint.hh"
#include "slice_stream.hh"
#include <algorithm>

namespace litecore {
    using namespace std;
    using namespace fleece;


#pragma mark - VERSION:

    Version::Version(slice ascii, peerID myPeerID) {
        if ( !_readASCII(ascii) ) throwBadASCII(ascii);
        if ( _author == myPeerID ) _author = kMePeerID;  // Abbreviate my ID
    }

    Version::Version(slice_istream& data) {
        optional<uint64_t> gen = data.readUVarInt(), id = data.readUVarInt();
        if ( !gen || !id ) throwBadBinary();
        _gen       = *gen;
        _author.id = *id;
        validate();
    }

    /*static*/ optional<Version> Version::readASCII(slice ascii, peerID myPeerID) {
        Version vers;
        if ( !vers._readASCII(ascii) ) return nullopt;
        if ( vers._author == myPeerID ) vers._author = kMePeerID;  // Abbreviate my ID
        return vers;
    }

    bool Version::_readASCII(slice ascii) noexcept {
        slice_istream in = ascii;
        _gen             = in.readHex();
        if ( in.readByte() != '@' || _gen == 0 ) return false;
        if ( in.peekByte() == '*' ) {
            in.readByte();
            _author = kMePeerID;
        } else {
            _author.id = in.readHex();
            if ( _author.id == 0 || _author == kMePeerID ) return false;
        }
        return (in.size == 0);
    }

    void Version::validate() const {
        if ( _gen == 0 ) error::_throw(error::BadRevisionID);
    }

    bool Version::writeBinary(slice_ostream& out, peerID myID) const {
        uint64_t id = (_author == kMePeerID) ? myID.id : _author.id;
        return out.writeUVarInt(_gen) && out.writeUVarInt(id);
    }

    bool Version::writeASCII(slice_ostream& out, peerID myID) const {
        if ( !out.writeHex(_gen) || !out.writeByte('@') ) return false;
        auto author = (_author != kMePeerID) ? _author : myID;
        if ( author != kMePeerID ) return out.writeHex(author.id);
        else
            return out.writeByte('*');
    }

    alloc_slice Version::asASCII(peerID myID) const {
        auto result
                = slice_ostream::alloced(kMaxASCIILength, [&](slice_ostream& out) { return writeASCII(out, myID); });
        Assert(result);
        return result;
    }

    versionOrder Version::compareGen(generation a, generation b) {
        if ( a > b ) return kNewer;
        else if ( a < b )
            return kOlder;
        return kSame;
    }

    void Version::throwBadBinary() { error::_throw(error::BadRevisionID, "Invalid binary version ID"); }

    void Version::throwBadASCII(slice string) {
        if ( string ) error::_throw(error::BadRevisionID, "Invalid version string '%.*s'", SPLAT(string));
        else
            error::_throw(error::BadRevisionID, "Invalid version string");
    }


}  // namespace litecore
