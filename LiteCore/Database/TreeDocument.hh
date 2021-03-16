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
#include "DocumentFactory.hh"
#include "fleece/Fleece.h"

namespace litecore {

    /** DocumentFactory subclass for rev-tree document schema. */
    class TreeDocumentFactory final : public DocumentFactory {
    public:
        TreeDocumentFactory(DatabaseImpl *db)   :DocumentFactory(db) { }
        Retained<C4Document> newDocumentInstance(slice docID, ContentOption) override;
        Retained<C4Document> newDocumentInstance(const Record&) override;
        bool isFirstGenRevID(slice revID) const override;

        std::vector<alloc_slice> findAncestors(const std::vector<slice> &docIDs, const std::vector<slice> &revIDs,
                                               unsigned maxAncestors, bool mustHaveBodies,
                                               C4RemoteID remoteDBID) override;

        static C4Document* documentContaining(FLValue value);
    };

}
