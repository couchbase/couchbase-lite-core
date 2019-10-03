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
#include "IncomingRev.hh"
#include "SecureRandomize.hh"
#include "StringUtil.hh"
#include "c4DocEnumerator.h"
#include <sstream>

using namespace std;

namespace litecore { namespace repl { namespace tuning {
    size_t kMinBodySizeForDelta = 200;
}}}

namespace litecore { namespace repl {

#pragma mark DOCIDMULTISET:


    bool DocIDMultiset::contains(const fleece::alloc_slice &docID) const {
        return _set.use<bool>([&](const multiset &s) {
            return s.count(docID) > 0;
        });
    }

    void DocIDMultiset::add(const fleece::alloc_slice &docID) {
        _set.use([&](multiset &s) {
            s.insert(docID);
        });
    }

    void DocIDMultiset::remove(const fleece::alloc_slice &docID) {
        _set.use([&](multiset &s) {
            auto i = s.find(docID);
            if (i != s.end())
                s.erase(i);
        });
    }


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


    void RevToSend::trim() {
        remoteAncestorRevID.reset();
        ancestorRevIDs.reset();
    }


    string RevToSend::historyString(C4Document *doc) {
        int nWritten = 0;
        stringstream historyStream;
        string::size_type lastPos = 0;

        auto append = [&](slice revID) {
            lastPos = (string::size_type)historyStream.tellp();
            if (nWritten++ > 0)
                historyStream << ',';
            historyStream.write((const char*)revID.buf, revID.size);
        };

        auto removeLast = [&]() {
            string buf = historyStream.str();
            buf.resize(lastPos);
            historyStream.str(buf);
            historyStream.seekp(lastPos);
            --nWritten;
        };

        // Go back through history, starting with the desired rev's parent, until we either reach
        // a rev known to the peer or we run out of history. Do not write more than `maxHistory`
        // revisions, but always write the rev known to the peer if there is one.
        // There may be gaps in the history (non-consecutive generations) if revs have been pruned.
        // If sending these, make up random revIDs for them since they don't matter.
        Assert(c4doc_selectRevision(doc, revID, true, nullptr));
        unsigned lastGen = c4rev_getGeneration(doc->selectedRev.revID);
        while (c4doc_selectParentRevision(doc)) {
            slice revID = doc->selectedRev.revID;
            unsigned gen = c4rev_getGeneration(revID);
            while (gen < --lastGen && nWritten < maxHistory) {
                // We don't have this revision (the history got deeper than the local db's
                // maxRevTreeDepth), so make up a random revID. The server probably won't care.
                append(slice(format("%u-faded000%.08x%.08x",
                                    lastGen, RandomNumber(), RandomNumber())));
            }
            lastGen = gen;

            if (hasRemoteAncestor(revID)) {
                // Always write the common ancestor, making room if necessary:
                if (nWritten == maxHistory)
                    removeLast();
                append(revID);
                break;
            } else {
                if (nWritten < maxHistory)
                    append(revID);
            }
        }
        return historyStream.str();
    }


#pragma mark - REVTOINSERT:


    RevToInsert::~RevToInsert()
    { }


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
