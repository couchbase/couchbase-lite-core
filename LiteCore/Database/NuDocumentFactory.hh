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

        virtual Retained<Document> newDocumentInstance(C4Slice docID) override;
        virtual Retained<Document> newDocumentInstance(const Record&) override;

        virtual Retained<Document> newLeafDocumentInstance(C4Slice docID, C4Slice revID,
                                                           bool withBody) override;

        virtual alloc_slice revIDFromVersion(slice version) override;
        virtual bool isFirstGenRevID(slice revID) override          {return false;}

        virtual vector<alloc_slice> findAncestors(const vector<slice> &docIDs,
                                                  const vector<slice> &revIDs,
                                                  unsigned maxAncestors,
                                                  bool mustHaveBodies,
                                                  C4RemoteID remoteDBID) override;

    };

}
