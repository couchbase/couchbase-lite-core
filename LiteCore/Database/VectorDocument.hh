//
// VectorDocument.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "Document.hh"

namespace c4Internal {

    class VectorDocumentFactory : public DocumentFactory {
    public:
        VectorDocumentFactory(Database *db)   :DocumentFactory(db) { }

        Retained<Document> newDocumentInstance(C4Slice docID, ContentOption) override;
        Retained<Document> newDocumentInstance(const Record&) override;

        Retained<Document> newLeafDocumentInstance(C4Slice docID, C4Slice revID,
                                                   bool withBody) override;
        
        std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs,
                                               const std::vector<slice> &revIDs,
                                               unsigned maxAncestors,
                                               bool mustHaveBodies,
                                               C4RemoteID remoteDBID) override;

        static Document* documentContaining(FLValue value);

    };

}
