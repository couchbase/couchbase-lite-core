//
//  RawRevTree.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 5/25/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include "RevTree.hh"
#include "KeyStore.hh"
#include "Endian.hh"
#include <deque>
#include <vector>


namespace litecore {

#pragma pack(1)

    // Layout of a single revision in encoded form. Rev tree is stored as a sequence of these
    // followed by a 32-bit zero.
    // Revs are stored in decending priority, with the current leaf rev(s) coming first.
    class RawRevision {
    public:
        static std::deque<Rev> decodeTree(slice raw_tree,
                                                RevTree *owner NONNULL,
                                                sequence_t curSeq);

        static alloc_slice encodeTree(const std::vector<Rev*> &revs);

        static slice getCurrentRevBody(slice raw_tree) noexcept;

    private:
        static const uint16_t kNoParent = UINT16_MAX;

        // Private RevisionFlags bits used in encoded form:
        enum : uint8_t {
            kHasData = 0x80,  /**< Does this raw rev contain JSON/Fleece data? */
            kNonPersistentFlags  = (Rev::kNew),         // Not saved to disk
            kPersistentOnlyFlags = (kHasData),          // Only used on disk, not in memory
        };

        uint32_t        size_BE;        // Total size of this tree rev (big-endian)
        uint16_t        parentIndex_BE; // Index in list of parent, or kNoParent if none
        uint8_t         flags;
        uint8_t         revIDLen;
        char            revID[1];       // actual size is [revIDLen]
        // These follow the revID:
        // varint       sequence
        // if HasData flag:
        //    char      data[];         // Contains the revision body (JSON)

        bool isValid() const {
            return size_BE != 0;
        }

        slice body() const;

        const RawRevision *next() const {
            return (const RawRevision*)fleece::offsetby(this, _dec32(size_BE));
        }

        unsigned count() const {
            unsigned count = 0;
            for (const RawRevision *rev = this; rev->isValid(); rev = rev->next())
                ++count;
            return count;
        }

        static size_t sizeToWrite(const Rev&);
        void copyTo(Rev &dst, const std::deque<Rev>&) const;
        RawRevision* copyFrom(const Rev &rev);
    };

#pragma pack()
    
}
