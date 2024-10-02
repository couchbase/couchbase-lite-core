//
// ReplicatedRev.hh
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/RefCounted.hh"
#include "fleece/slice.hh"
#include "c4DocumentTypes.h"
#include "c4ReplicatorTypes.h"
#include "Error.hh"

namespace litecore::repl {

    enum class Dir { kPulling = 0, kPushing };

    enum class RevocationMode : uint8_t { kNone, kRevokedAccess, kRemovedFromChannel };

    /** Metadata of a document revision. Abstract superclass of RevToSend, RevToInsert (see
        ReplicatorTypes.hh). Used to track revisions during the replication flow, and to notify
        the delegate at the end. */
    class ReplicatedRev : public fleece::RefCounted {
      public:
        using slice       = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        const alloc_slice collectionName = {};  // TODO: Collection aware
        const alloc_slice scopeName      = {};
        // Note: The following fields, up to collectionContext, must be compatible with the public
        // C4DocumentEnded struct:
        const C4CollectionSpec collectionSpec = {collectionName, scopeName};
        const alloc_slice      docID;
        const alloc_slice      revID;
        C4RevisionFlags        flags{0};
        C4SequenceNumber       sequence;
        C4Error                error{};
        bool                   errorIsTransient{false};
        void*                  collectionContext{nullptr};

        const C4DocumentEnded* asDocumentEnded() const {
            auto c4de = (const C4DocumentEnded*)&collectionSpec;
            // Verify the abovementioned compatibility:
            DebugAssert((void*)&c4de->docID == &docID);
            DebugAssert((void*)&c4de->collectionContext == &collectionContext);
            return c4de;
        }

        bool isWarning{false};
        bool rejectedByRemote{false};

        virtual Dir dir() const = 0;

        bool deleted() const { return (flags & kRevDeleted) != 0; }

        // Internal use only:
        virtual void trim() = 0;

      protected:
        ReplicatedRev(C4CollectionSpec collSpec, slice docID_, slice revID_, void* collectionContext_,
                      C4SequenceNumber sequence_ = {})
            : collectionName(collSpec.name)
            , scopeName(collSpec.scope)
            , docID(alloc_slice::nullPaddedString(docID_))
            , revID(alloc_slice::nullPaddedString(revID_))
            , sequence(sequence_)
            , collectionContext(collectionContext_) {}

        ~ReplicatedRev() override = default;
    };

#ifdef __GNUC__
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
    static_assert(offsetof(ReplicatedRev, errorIsTransient) - offsetof(ReplicatedRev, docID)
                          == offsetof(C4DocumentEnded, errorIsTransient) - offsetof(C4DocumentEnded, docID),
                  "ReplicatedRev doesn't match C4DocumentEnded");
#ifdef __GNUC__
#    pragma GCC diagnostic pop
#endif

}  // namespace litecore::repl
