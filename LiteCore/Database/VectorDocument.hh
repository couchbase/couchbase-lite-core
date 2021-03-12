//
// VectorDocument.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "Document.hh"
#include <vector>

namespace c4Internal {

    class VectorDocumentFactory final : public DocumentFactory {
    public:
        VectorDocumentFactory(Database *db)   :DocumentFactory(db) { }

        Retained<Document> newDocumentInstance(C4Slice docID, ContentOption) override;
        Retained<Document> newDocumentInstance(const Record&) override;

        std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs,
                                               const std::vector<slice> &revIDs,
                                               unsigned maxAncestors,
                                               bool mustHaveBodies,
                                               C4RemoteID remoteDBID) override;

        static Document* documentContaining(FLValue value);

    };

}
