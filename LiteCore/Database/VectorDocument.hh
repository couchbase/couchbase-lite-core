//
// VectorDocument.hh
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "DocumentFactory.hh"
#include "fleece/FLBase.h"

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    class VectorDocumentFactory final : public DocumentFactory {
      public:
        explicit VectorDocumentFactory(C4Collection* db) : DocumentFactory(db) {}

        Retained<C4Document> newDocumentInstance(slice docID, ContentOption) override;
        Retained<C4Document> newDocumentInstance(const Record&) override;

        std::vector<alloc_slice> findAncestors(const std::vector<slice>& docIDs, const std::vector<slice>& revIDs,
                                               unsigned maxAncestors, bool mustHaveBodies,
                                               C4RemoteID remoteDBID) override;

        static C4Document* C4NULLABLE documentContaining(FLValue value);
    };

}  // namespace litecore

C4_ASSUME_NONNULL_END
