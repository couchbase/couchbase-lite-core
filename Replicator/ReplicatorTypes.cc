//
// ReplicatorTypes.cc
//
// Copyright (c) 2018 Couchbase, Inc All rights reserved.
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

#include "ReplicatorTypes.hh"
#include "make_unique.h"

using namespace std;

namespace litecore { namespace repl {

    RevToSend::RevToSend(const C4DocumentInfo &info, const alloc_slice &remoteAncestor)
    :Rev(info.docID, info.revID, info.sequence, info.bodySize)
    ,remoteAncestorRevID(remoteAncestor)
    {
        flags = c4rev_flagsFromDocFlags(info.flags);
    }


    void RevToSend::addRemoteAncestor(slice revID) {
        if (!revID)
            return;
        if (!ancestorRevIDs)
            ancestorRevIDs = make_unique<set<alloc_slice>>();
        ancestorRevIDs->emplace(revID);
    }


    bool RevToSend::hasRemoteAncestor(slice revID) const {
        if (revID == remoteAncestorRevID)
            return true;
        if (ancestorRevIDs) {
            alloc_slice& fakeRevID = *(alloc_slice*)&revID; // work around type issues in std::set
            if (ancestorRevIDs->find(fakeRevID) != ancestorRevIDs->end())
                return true;
        }
        return false;
    }


    RevToInsert::RevToInsert(slice docID_, slice revID_,
                             slice historyBuf_,
                             bool deleted_,
                             bool noConflicts_)
    :Rev(docID_, revID_)
    ,historyBuf(historyBuf_)
    {
        if (deleted_)
            flags |= kRevDeleted;
        noConflicts = noConflicts_;
    }

} }
