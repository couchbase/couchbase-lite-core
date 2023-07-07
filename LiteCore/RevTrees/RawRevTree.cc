//
// RawRevTree.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "RawRevTree.hh"
#include "RevTree.hh"
#include "Error.hh"
#include "varint.hh"
#include "NumConversion.hh"

using namespace std;
using namespace fleece;

namespace litecore {

#pragma pack(1)

    struct RemoteEntry {
        uint16_t remoteDBID_BE;
        uint16_t revIndex_BE;
    };

#pragma pack()

    bool RawRevision::isRevTree(slice raw_tree) {
        // The data cannot be shorter than a single revision:
        if ( raw_tree.size < sizeof(RawRevision) ) return false;
        auto end = (const RawRevision*)raw_tree.end();
        // Check each revision:
        const RawRevision* next;
        for ( auto rawRev = (const RawRevision*)raw_tree.buf; rawRev < end; rawRev = next ) {
            next = rawRev->next();
            if ( next == rawRev ) return true;  // This is the end-of-tree marker (a 0 size).
            if ( next <= (void*)&rawRev->revID[rawRev->revIDLen] ) return false;  // Rev is too short for its data:
        }
        return false;  // Fell off end before finding end marker
    }

    std::deque<Rev> RawRevision::decodeTree(slice raw_tree, RevTree::RemoteRevMap& remoteMap,
                                            std::vector<const Rev*>& rejectedRevs, RevTree* owner, sequence_t curSeq) {
        auto rawRev = (const RawRevision*)raw_tree.buf;
        if ( fleece::endian::dec32(rawRev->size_BE) > raw_tree.size )
            error::_throw(error::CorruptRevisionData, "RawRevision decodeTree binary error");
        unsigned count = rawRev->count();
        if ( count > UINT16_MAX )
            error::_throw(error::CorruptRevisionData, "RawRevision decodeTree reading count error");
        deque<Rev> revs(count);
        auto       rev = revs.begin();
        for ( ; rawRev->isValid(); rawRev = rawRev->next() ) {
            rawRev->copyTo(*rev, revs);
            if ( rev->sequence == 0_seq ) rev->sequence = curSeq;
            rev->owner = owner;
            rev++;
        }

        auto entry = (const RemoteEntry*)offsetby(rawRev, sizeof(uint32_t));
        while ( entry < (const void*)raw_tree.end() ) {
            RevTree::RemoteID remoteID = endian::dec16(entry->remoteDBID_BE);
            auto              revIndex = endian::dec16(entry->revIndex_BE);
            // 0/0 is the zero mark that separates entries of remoteRevMap from entries of rejectedRevs
            // c.f. the comment in encodeTree
            if ( remoteID == 0 && revIndex == 0 ) {
                ++entry;
                break;  // The zero mark
            }
            if ( remoteID == 0 || revIndex >= count )
                error::_throw(error::CorruptRevisionData, "RawRevision dcodeTree revIndex error at remoteMap");
            remoteMap[remoteID] = &revs[revIndex];
            ++entry;
        }

        while ( entry < (const void*)raw_tree.end() ) {
            RevTree::RemoteID remoteID = endian::dec16(entry->remoteDBID_BE);
            auto              revIndex = endian::dec16(entry->revIndex_BE);
            if ( remoteID != 0 || revIndex >= count )
                error::_throw(error::CorruptRevisionData, "RawRevision decodeTree revIndex error at rejectedRevs");
            rejectedRevs.push_back(&revs[revIndex]);
            ++entry;
        }

        if ( (uint8_t*)entry != (uint8_t*)raw_tree.end() ) {
            error::_throw(error::CorruptRevisionData, "RawRevision decodeTree binary layout error");
        }
        return revs;
    }

    alloc_slice RawRevision::encodeTree(const vector<Rev*>& revs, const RevTree::RemoteRevMap& remoteMap,
                                        const std::vector<const Rev*>& rejectedRevs) {
        // Allocate output buffer:
        size_t totalSize = sizeof(uint32_t);  // start with space for trailing 0 size
        for ( Rev* rev : revs ) totalSize += sizeToWrite(*rev);
        totalSize += (remoteMap.size() + 1  // a zero mark in the middle
                      + rejectedRevs.size())
                     * sizeof(RemoteEntry);

        alloc_slice result(totalSize);

        // Write the raw revs:
        auto dst = (RawRevision*)result.buf;
        for ( Rev* src : revs ) { dst = dst->copyFrom(*src); }
        dst->size_BE = endian::enc32(0);  // write trailing 0 size marker

        auto entry = (RemoteEntry*)offsetby(dst, sizeof(uint32_t));
        for ( auto remote : remoteMap ) {
            entry->remoteDBID_BE = endian::enc16(uint16_t(remote.first));
            entry->revIndex_BE   = endian::enc16(uint16_t(remote.second->index()));
            ++entry;
        }

        // Zero mark: in order to be binary compatibale, we cannot change the layout of the
        // above entry. In order to separate the entries, we take the avantage of the fact
        // that, with remoteRevMap, remote index must not be zero, and artificially prepend
        // every entry of rejectedRevs by a zero. The first 0/0 separates the map and
        // the vector.
        entry->remoteDBID_BE = endian::enc16(uint16_t(0));
        entry->revIndex_BE   = endian::enc16(uint16_t(0));
        ++entry;

        for ( auto rejected : rejectedRevs ) {
            entry->remoteDBID_BE = endian::enc16(uint16_t(0));
            entry->revIndex_BE   = endian::enc16(uint16_t(rejected->index()));
            ++entry;
        }

        Assert(entry == (const void*)result.end());
        // Sanity check:
        auto     rawRev = (const RawRevision*)result.buf;
        unsigned count  = rawRev->count();
        // c.f. RawRevision::decodeTree
        if ( count > UINT16_MAX )
            error::_throw(error::UnexpectedError,
                          "RawRevision::encodeTree: too many revs in the revision tree. The limit is %u", UINT16_MAX);

        return result;
    }

    size_t RawRevision::sizeToWrite(const Rev& rev) {
        return offsetof(RawRevision, revID) + rev.revID.size + SizeOfVarInt(uint64_t(rev.sequence)) + rev._body.size;
    }

    RawRevision* RawRevision::copyFrom(const Rev& rev) {
        size_t revSize = sizeToWrite(rev);
        this->size_BE  = endian::enc32((uint32_t)revSize);
        this->revIDLen = (uint8_t)rev.revID.size;
        memcpy(this->revID, rev.revID.buf, rev.revID.size);
        this->parentIndex_BE = endian::enc16(uint16_t(rev.parent ? rev.parent->index() : kNoParent));

        uint8_t dstFlags = rev.flags & ~kNonPersistentFlags;
        if ( rev._body ) dstFlags |= RawRevision::kHasData;
        this->flags = (Rev::Flags)dstFlags;

        void* dstData = offsetby(&this->revID[0], narrow_cast<ptrdiff_t>(rev.revID.size));
        dstData       = offsetby(dstData, narrow_cast<ptrdiff_t>(PutUVarInt(dstData, uint64_t(rev.sequence))));
        rev._body.copyTo(dstData);

        return (RawRevision*)offsetby(this, narrow_cast<ptrdiff_t>(revSize));
    }

    void RawRevision::copyTo(Rev& dst, const deque<Rev>& revs) const {
        const void* end       = this->next();
        dst._hasInsertedRevID = false;
        dst._hasInsertedBody  = false;
        dst.revID             = {this->revID, this->revIDLen};
        dst.flags             = (Rev::Flags)(this->flags & ~kPersistentOnlyFlags);
        auto parentIndex      = endian::dec16(this->parentIndex_BE);
        if ( parentIndex == kNoParent ) dst.parent = nullptr;
        else
            dst.parent = &revs[parentIndex];
        const void* data = offsetby(&this->revID, this->revIDLen);
        ptrdiff_t   len  = (uint8_t*)end - (uint8_t*)data;
        data = offsetby(data, narrow_cast<ptrdiff_t>(GetUVarInt(slice(data, len), (uint64_t*)&dst.sequence)));
        if ( this->flags & RawRevision::kHasData ) dst._body = slice(data, end);
        else
            dst._body = nullslice;
    }

    slice RawRevision::body() const {
        if ( _usuallyTrue(this->flags & RawRevision::kHasData) ) {
            const void* end  = this->next();
            const void* data = offsetby(&this->revID, this->revIDLen);
            data             = SkipVarInt(data);
            return {data, end};
        } else {
            return nullslice;
        }
    }

}  // namespace litecore
