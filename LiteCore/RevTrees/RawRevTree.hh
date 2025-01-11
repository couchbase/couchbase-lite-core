//
// RawRevTree.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include "RevTree.hh"
#include "Endian.hh"
#include <deque>
#include <vector>

namespace litecore {

#pragma pack(1)

    // Layout of a single revision in encoded form. Rev tree is stored as a sequence of these
    // followed by a 32-bit zero.
    // Revs are stored in decending priority, with the current leaf rev(s) coming first.
    // Following the revs is a series of (remote DB ID, revision index) pairs that mark which
    // revision is the current one for every remote database.
    class RawRevision {
      public:
        /// Safe, quick check to determine if data is in rev-tree format.
        /// This can be used to distinguish a v2.x 'body' column, which is a rev-tree,
        /// from v3.x where it's Fleece (and the rev-tree is in 'extra'.)
        static bool isRevTree(slice raw_tree);

        static std::deque<Rev> decodeTree(slice raw_tree, RevTree::RemoteRevMap& remoteMap,
                                          std::vector<const Rev*>& rejectedRevs, RevTree* owner NONNULL,
                                          sequence_t curSeq);

        static alloc_slice encodeTree(const std::vector<Rev*>& revs, const RevTree::RemoteRevMap& remoteMap,
                                      const std::vector<const Rev*>& rejectedRevs);

        static slice getCurrentRevBody(slice raw_tree) noexcept {
            auto rawRev = (const RawRevision*)raw_tree.buf;
            return rawRev->body();
        }

      private:
        static const uint16_t kNoParent = UINT16_MAX;

        // Private RevisionFlags bits used in encoded form:
        enum : uint8_t {
            kHasData             = 0x80,         /**< Does this raw rev contain JSON/Fleece data? */
            kNonPersistentFlags  = (Rev::kNew),  // Not saved to disk
            kPersistentOnlyFlags = (kHasData),   // Only used on disk, not in memory
        };

        uint32_t size_BE;         // Total size of this tree rev (big-endian)
        uint16_t parentIndex_BE;  // Index in list of parent, or kNoParent if none
        uint8_t  flags;
        uint8_t  revIDLen;
        char     revID[1];  // actual size is [revIDLen]

        // These follow the revID:
        // varint       sequence
        // if HasData flag:
        //    char      data[];         // Contains the revision body (Fleece)

        [[nodiscard]] bool isValid() const { return size_BE != 0; }

        [[nodiscard]] slice body() const;

        [[nodiscard]] const RawRevision* next() const {
            return (const RawRevision*)fleece::offsetby(this, fleece::endian::dec32(size_BE));
        }

        [[nodiscard]] unsigned count() const {
            unsigned count = 0;
            for ( const RawRevision* rev = this; rev->isValid(); rev = rev->next() ) ++count;
            return count;
        }

        static size_t sizeToWrite(const Rev&);
        void          copyTo(Rev& dst, const std::deque<Rev>&) const;
        RawRevision*  copyFrom(const Rev& rev);
    };

#pragma pack()

}  // namespace litecore
