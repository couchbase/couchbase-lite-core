//
// VectorDocument.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "DocumentFactory.hh"
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN

namespace litecore {

    class VectorDocumentFactory final : public DocumentFactory {
    public:
        VectorDocumentFactory(DatabaseImpl *db)   :DocumentFactory(db) { }

        Retained<C4Document> newDocumentInstance(slice docID, ContentOption) override;
        Retained<C4Document> newDocumentInstance(const Record&) override;

        std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs,
                                               const std::vector<slice> &revIDs,
                                               unsigned maxAncestors,
                                               bool mustHaveBodies,
                                               C4RemoteID remoteDBID) override;

        static C4Document* documentContaining(FLValue value);

    };

}

C4_ASSUME_NONNULL_END
