//
//  TreeDocument.hh
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "Document.hh"

namespace c4Internal {
    class Document;

    /** DocumentFactory subclass for rev-tree document schema. */
    class TreeDocumentFactory : public DocumentFactory {
    public:
        TreeDocumentFactory(Database *db)   :DocumentFactory(db) { }
        Retained<Document> newDocumentInstance(C4Slice docID) override;
        Retained<Document> newDocumentInstance(const Record&) override;
        Retained<Document> newLeafDocumentInstance(C4Slice docID, C4Slice revID, bool withBody) override;
        alloc_slice revIDFromVersion(slice version) override;
        bool isFirstGenRevID(slice revID) override;
        static slice fleeceAccessor(slice docBody);

        std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs, const std::vector<slice> &revIDs,
                                               unsigned maxAncestors, bool mustHaveBodies,
                                               C4RemoteID remoteDBID) override;

        static Document* documentContaining(const fleece::impl::Value *value) {
            auto doc = treeDocumentContaining(value);
            return doc ? doc : leafDocumentContaining(value);
        }


    private:
        static Document* treeDocumentContaining(const fleece::impl::Value *value);
        static Document* leafDocumentContaining(const fleece::impl::Value *value);
    };

}
