//
// ReplicatorTypes.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ReplicatorTypes.hh"
#include "IncomingRev.hh"
#include "ReplicatorTuning.hh"
#include "c4DocEnumeratorTypes.h"
#include "c4Document.hh"
#include <sstream>

using namespace std;

namespace litecore::repl::tuning {
    size_t kMinBodySizeForDelta = 200;
}  // namespace litecore::repl::tuning

namespace litecore::repl {

#pragma mark - REVTOSEND:

    RevToSend::RevToSend(const C4DocumentInfo& info, C4CollectionSpec collSpec, void* context)
        : ReplicatedRev(collSpec, slice(info.docID), slice(info.revID), context, info.sequence)
        , bodySize(info.bodySize)
        , expiration(info.expiration) {
        flags = C4Document::revisionFlagsFromDocFlags(info.flags);
    }

    void RevToSend::addRemoteAncestor(slice revID) {
        if ( !revID ) return;
        if ( !ancestorRevIDs ) ancestorRevIDs = make_unique<vector<alloc_slice>>();
        ancestorRevIDs->emplace_back(revID);
    }

    bool RevToSend::hasRemoteAncestor(slice revID) const {
        if ( revID == remoteAncestorRevID ) return true;
        if ( ancestorRevIDs ) {
            for ( const alloc_slice& anc : *ancestorRevIDs )
                if ( anc == revID ) return true;
        }
        return false;
    }

    void RevToSend::trim() {
        remoteAncestorRevID.reset();
        ancestorRevIDs.reset();
    }

    alloc_slice RevToSend::historyString(C4Document* doc) {
        const alloc_slice* ancestors     = nullptr;
        size_t             ancestorCount = 0;
        if ( ancestorRevIDs ) {
            if ( remoteAncestorRevID ) ancestorRevIDs->push_back(remoteAncestorRevID);
            ancestors     = ancestorRevIDs->data();
            ancestorCount = ancestorRevIDs->size();
        } else if ( remoteAncestorRevID ) {
            ancestors     = &remoteAncestorRevID;
            ancestorCount = 1;
        }
        alloc_slice result = doc->getRevisionHistory(maxHistory, (const slice*)ancestors, unsigned(ancestorCount));
        if ( ancestorRevIDs && remoteAncestorRevID ) {
            // Undo the push_back above
            ancestorRevIDs->resize(ancestorCount - 1);
        }
        return result;
    }

#pragma mark - REVTOINSERT:


    RevToInsert::~RevToInsert() = default;

    RevToInsert::RevToInsert(IncomingRev* owner_, slice docID_, slice revID_, slice historyBuf_, bool deleted_,
                             bool noConflicts_, C4CollectionSpec spec, void* collectionContext)
        : ReplicatedRev(spec, docID_, revID_, collectionContext)
        , historyBuf(historyBuf_)
        , noConflicts(noConflicts_)
        , owner(owner_) {
        if ( deleted_ ) flags |= kRevDeleted;
    }

    RevToInsert::RevToInsert(slice docID_, slice revID_, RevocationMode mode, C4CollectionSpec spec,
                             void* collectionContext)
        : ReplicatedRev(spec, std::move(docID_), std::move(revID_), collectionContext), revocationMode(mode) {
        flags |= kRevPurged;
    }

    void RevToInsert::trimBody() {
        doc = nullptr;
        historyBuf.reset();
        deltaSrc.reset();
        deltaSrcRevID.reset();
        _curVersAlloc.reset();
    }

    void RevToInsert::trim() {
        trimBody();
        owner = nullptr;
    }

    vector<C4String> RevToInsert::history() {
        vector<C4String> history;
        history.reserve(10);

        // For version vector, the merged versions if exists will appear
        // in the history property of the rev message. The VersionVector class
        // used for parsing the history expects the current version to be
        // together with its merged version in the cv, mv, mv; format.
        // This code below will check for the merge versions from the history
        // and put them together with the current version (revID).

        const void* pos = historyBuf.buf;
        auto        sc  = historyBuf.findByte(';');  // RevTree will not have ';'.
        if ( sc ) {
            while ( pos < sc && *(char*)pos == ' ' ) pos = (char*)pos + 1;
            _curVersAlloc = revID;
            _curVersAlloc.append(", ");
            _curVersAlloc.append(slice(pos, sc + 1));
            history.push_back(_curVersAlloc);
            pos = (char*)sc + 1;
        } else {
            history.push_back(revID);
        }

        for ( const void* end = historyBuf.end(); pos < end; ) {
            while ( pos < end && *(char*)pos == ' ' ) pos = (char*)pos + 1;
            auto comma = slice(pos, end).findByteOrEnd(',');
            history.push_back(slice(pos, comma));
            pos = comma + 1;
        }
        return history;
    }


}  // namespace litecore::repl
