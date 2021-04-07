//
// RawRevTree.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#include "RawRevTree.hh"
#include "RevTree.hh"
#include "Error.hh"
#include "varint.hh"

using namespace std;
using namespace fleece;


namespace litecore {

#pragma pack(1)
    struct RemoteEntry {
        uint16_t remoteDBID_BE;
        uint16_t revIndex_BE;
    };
#pragma pack()


    std::deque<Rev> RawRevision::decodeTree(slice raw_tree,
                                            RevTree::RemoteRevMap &remoteMap,
                                            RevTree* owner,
                                            sequence_t curSeq)
    {
        const RawRevision *rawRev = (const RawRevision*)raw_tree.buf;
        if (fleece::endian::dec32(rawRev->size_BE) > raw_tree.size)
            error::_throw(error::CorruptRevisionData);
        unsigned count = rawRev->count();
        if (count > UINT16_MAX)
            error::_throw(error::CorruptRevisionData);
        deque<Rev> revs(count);
        auto rev = revs.begin();
        for (; rawRev->isValid(); rawRev = rawRev->next()) {
            rawRev->copyTo(*rev, revs);
            if (rev->sequence == 0)
                rev->sequence = curSeq;
            rev->owner = owner;
            rev++;
        }

        auto entry = (const RemoteEntry*)offsetby(rawRev, sizeof(uint32_t));
        while (entry < (const void*)raw_tree.end()) {
            RevTree::RemoteID remoteID = endian::dec16(entry->remoteDBID_BE);
            auto revIndex = endian::dec16(entry->revIndex_BE);
            if (remoteID == 0 || revIndex >= count)
                error::_throw(error::CorruptRevisionData);
            remoteMap[remoteID] = &revs[revIndex];
            ++entry;
        }

        if ((uint8_t*)entry != (uint8_t*)raw_tree.end()) {
            error::_throw(error::CorruptRevisionData);
        }
        return revs;
    }


    alloc_slice RawRevision::encodeTree(const vector<Rev*> &revs,
                                        const RevTree::RemoteRevMap &remoteMap)
    {
        // Allocate output buffer:
        size_t totalSize = sizeof(uint32_t);  // start with space for trailing 0 size
        for (Rev *rev : revs)
            totalSize += sizeToWrite(*rev);
        totalSize += remoteMap.size() * sizeof(RemoteEntry);

        alloc_slice result(totalSize);

        // Write the raw revs:
        RawRevision *dst = (RawRevision*)result.buf;
        for (Rev *src : revs) {
            dst = dst->copyFrom(*src);
        }
        dst->size_BE = endian::enc32(0);   // write trailing 0 size marker

        auto entry = (RemoteEntry*)offsetby(dst, sizeof(uint32_t));
        for (auto remote : remoteMap) {
            entry->remoteDBID_BE = endian::enc16(uint16_t(remote.first));
            entry->revIndex_BE = endian::enc16(uint16_t(remote.second->index()));
            ++entry;
        }

        Assert(entry == (const void*)result.end());
        return result;
    }


    size_t RawRevision::sizeToWrite(const Rev &rev) {
        return offsetof(RawRevision, revID)
             + rev.revID.size
             + SizeOfVarInt(rev.sequence)
             + rev._body.size;
    }

    RawRevision* RawRevision::copyFrom(const Rev &rev) {
        size_t revSize = sizeToWrite(rev);
        this->size_BE = endian::enc32((uint32_t)revSize);
        this->revIDLen = (uint8_t)rev.revID.size;
        memcpy(this->revID, rev.revID.buf, rev.revID.size);
        this->parentIndex_BE = endian::enc16(uint16_t(rev.parent ? rev.parent->index() : kNoParent));

        uint8_t dstFlags = rev.flags & ~kNonPersistentFlags;
        if (rev._body)
            dstFlags |= RawRevision::kHasData;
        this->flags = (Rev::Flags)dstFlags;

        void *dstData = offsetby(&this->revID[0], rev.revID.size);
        dstData = offsetby(dstData, PutUVarInt(dstData, rev.sequence));
        memcpy(dstData, rev._body.buf, rev._body.size);

        return (RawRevision*)offsetby(this, revSize);
    }

    void RawRevision::copyTo(Rev &dst, const deque<Rev> &revs) const {
        const void* end = this->next();
        dst.revID = {this->revID, this->revIDLen};
        dst.flags = (Rev::Flags)(this->flags & ~kPersistentOnlyFlags);
        auto parentIndex = endian::dec16(this->parentIndex_BE);
        if (parentIndex == kNoParent)
            dst.parent = nullptr;
        else
            dst.parent = &revs[parentIndex];
        const void *data = offsetby(&this->revID, this->revIDLen);
        ptrdiff_t len = (uint8_t*)end-(uint8_t*)data;
        data = offsetby(data, GetUVarInt(slice(data, len), &dst.sequence));
        if (this->flags & RawRevision::kHasData)
            dst._body = slice(data, end);
        else
            dst._body = nullslice;
    }


    slice RawRevision::body() const {
        if (_usuallyTrue(this->flags & RawRevision::kHasData)) {
            const void* end = this->next();
            const void *data = offsetby(&this->revID, this->revIDLen);
            data = SkipVarInt(data);
            return slice(data, end);
        } else {
            return nullslice;
        }
    }

}
