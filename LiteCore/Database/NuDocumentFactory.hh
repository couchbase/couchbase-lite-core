//
// NuDocumentFactory.hh
//
// Copyright © 2020 Couchbase. All rights reserved.
//

#pragma once
#include "Document.hh"

namespace c4Internal {

    class NuDocumentFactory : public DocumentFactory {
    public:
        NuDocumentFactory(Database *db)   :DocumentFactory(db) { }

        Retained<Document> newDocumentInstance(C4Slice docID) override;
        Retained<Document> newDocumentInstance(const Record&) override;

        Retained<Document> newLeafDocumentInstance(C4Slice docID, C4Slice revID,
                                                   bool withBody) override;

        alloc_slice revIDFromVersion(slice version) const override;
        bool isFirstGenRevID(slice revID) const override          {return false;}
        slice fleeceAccessor(slice docBody) const override;

        vector<alloc_slice> findAncestors(const vector<slice> &docIDs,
                                          const vector<slice> &revIDs,
                                          unsigned maxAncestors,
                                          bool mustHaveBodies,
                                          C4RemoteID remoteDBID) override;

    };

}
