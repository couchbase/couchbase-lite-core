//
// DocumentFactory.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Base.hh"
#include "c4DocumentTypes.h"
#include "Record.hh"
#include <vector>

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    /** Abstract interface for creating Document instances; owned by a Database. */
    class DocumentFactory : protected C4Base {
      public:
        using ContentOption = litecore::ContentOption;
        using Record        = litecore::Record;

        explicit DocumentFactory(C4Collection* coll) : _coll(coll) {}

        virtual ~DocumentFactory() = default;

        [[nodiscard]] C4Collection* collection() const { return _coll; }

        [[nodiscard]] virtual bool isFirstGenRevID(slice revID) const { return false; }

        [[nodiscard]] virtual alloc_slice generateDocRevID(slice body, slice parentRevID, bool deleted) = 0;

        virtual Retained<C4Document> newDocumentInstance(slice docID, ContentOption) = 0;
        virtual Retained<C4Document> newDocumentInstance(const Record&)              = 0;

        virtual std::vector<alloc_slice> findAncestors(const std::vector<slice>& docIDs,
                                                       const std::vector<slice>& revIDs, unsigned maxAncestors,
                                                       bool mustHaveBodies, C4RemoteID remoteDBID) = 0;

      private:
        C4Collection* const _coll;  // Unretained, to avoid ref-cycle
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
