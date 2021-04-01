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
#include "c4Document.hh"
#include "c4DocEnumerator.hh"
#include "IncomingRev.hh"
#include "SecureRandomize.hh"
#include "StringUtil.hh"
#include <sstream>

using namespace std;

namespace litecore { namespace repl { namespace tuning {
    size_t kMinBodySizeForDelta = 200;
}}}

namespace litecore { namespace repl {

#pragma mark - REVTOSEND:


    RevToSend::RevToSend(const C4DocumentInfo &info)
    :ReplicatedRev(slice(info.docID), slice(info.revID), info.sequence)
    ,bodySize(info.bodySize)
    ,expiration(info.expiration)
    {
        flags = c4rev_flagsFromDocFlags(info.flags);
    }


    void RevToSend::addRemoteAncestor(slice revID) {
        if (!revID)
            return;
        if (!ancestorRevIDs)
            ancestorRevIDs = make_unique<vector<alloc_slice>>();
        ancestorRevIDs->emplace_back(revID);
    }


    bool RevToSend::hasRemoteAncestor(slice revID) const {
        if (revID == remoteAncestorRevID)
            return true;
        if (ancestorRevIDs) {
            for (const alloc_slice &anc : *ancestorRevIDs)
                if (anc == revID)
                    return true;
        }
        return false;
    }


    void RevToSend::trim() {
        remoteAncestorRevID.reset();
        ancestorRevIDs.reset();
    }


    alloc_slice RevToSend::historyString(C4Document *doc) {
        const alloc_slice* ancestors = nullptr;
        size_t ancestorCount = 0;
        if (ancestorRevIDs) {
            if (remoteAncestorRevID)
                ancestorRevIDs->push_back(remoteAncestorRevID);
            ancestors = ancestorRevIDs->data();
            ancestorCount = ancestorRevIDs->size();
        } else if (remoteAncestorRevID) {
            ancestors = &remoteAncestorRevID;
            ancestorCount = 1;
        }
        alloc_slice result = doc->getRevisionHistory(maxHistory,
                                                     (const slice*)ancestors,
                                                     unsigned(ancestorCount));
        if (ancestorRevIDs && remoteAncestorRevID) {
            // Undo the push_back above
            ancestorRevIDs->resize(ancestorCount - 1);
        }
        return result;
    }


#pragma mark - REVTOINSERT:


    RevToInsert::~RevToInsert() =default;


    RevToInsert::RevToInsert(IncomingRev* owner_,
                             slice docID_, slice revID_,
                             slice historyBuf_,
                             bool deleted_,
                             bool noConflicts_)
    :ReplicatedRev(docID_, revID_)
    ,historyBuf(historyBuf_)
    ,owner(owner_)
    ,noConflicts(noConflicts_)
    {
        if (deleted_)
            flags |= kRevDeleted;
    }


    void RevToInsert::trimBody() {
        doc = nullptr;
        historyBuf.reset();
        deltaSrc.reset();
        deltaSrcRevID.reset();
    }

    void RevToInsert::trim() {
        trimBody();
        owner = nullptr;
    }


    vector<C4String> RevToInsert::history() {
        vector<C4String> history;
        history.reserve(10);
        history.push_back(revID);
        for (const void *pos=historyBuf.buf, *end = historyBuf.end(); pos < end;) {
            auto comma = slice(pos, end).findByteOrEnd(',');
            history.push_back(slice(pos, comma));
            pos = comma + 1;
        }
        return history;
    }


} }
