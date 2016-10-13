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
#include "forestdb_endian.h"


namespace litecore {

    // Layout of a single revision in encoded form. Rev tree is stored as a sequence of these
    // followed by a 32-bit zero.
    // Revs are stored in decending priority, with the current leaf rev(s) coming first.
    class RawRevision {
    public:
        static std::vector<Rev> decodeTree(slice raw_tree,
                                                RevTree *owner,
                                                sequence curSeq);

        static alloc_slice encodeTree(const std::vector<Rev> &revs);

    private:
        // Private RevisionFlags bits used in encoded form:
        enum : uint8_t {
            kPublicPersistentFlags = (Rev::kLeaf | Rev::kDeleted | Rev::kHasAttachments),
            kHasBodyOffset = 0x40,  /**< Does this raw rev have a file position (oldBodyOffset)? */
            kHasData       = 0x80,  /**< Does this raw rev contain JSON data? */
        };

        uint32_t        size;           // Total size of this tree rev
        uint16_t        parentIndex;
        uint8_t         flags;
        uint8_t         revIDLen;
        char            revID[1];       // actual size is [revIDLen]
        // These follow the revID:
        // varint       sequence
        // if HasData flag:
        //    char      data[];       // Contains the revision body (JSON)
        // else:
        //    varint    oldBodyOffset;  // Points to record that has the body (0 if none)
        //    varint    body_size;

        bool isValid() const {
            return size != 0;
        }

        const RawRevision *next() const {
            return (const RawRevision*)fleece::offsetby(this, _dec32(size));
        }

        unsigned count() const {
            unsigned count = 0;
            for (const RawRevision *rev = this; rev->isValid(); rev = rev->next())
                ++count;
            return count;
        }

        static size_t sizeToWrite(const Rev&);
        void copyTo(Rev &dst) const;
        RawRevision* copyFrom(const Rev &rev);
    };
    
}
