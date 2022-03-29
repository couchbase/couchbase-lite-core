//
// c4ConnectedClientImpl.cc
//
// Copyright 2022-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4ConnectedClientImpl.hh"
#include "Replicator.hh"

using namespace fleece;

namespace litecore::client {

    alloc_slice C4ConnectedClientImpl::effectiveURL(slice url) {
        string newPath = string(url);
        if (!url.hasSuffix("/"_sl))
            newPath += "/";
        newPath += "_blipsync";
        return alloc_slice(newPath);
    }

    alloc_slice C4ConnectedClientImpl::socketOptions() {
        fleece::Encoder enc;
        enc.beginDict();
        enc.writeKey(C4STR(kC4SocketOptionWSProtocols));
        enc.writeString(repl::Replicator::ProtocolName().c_str());
        enc.endDict();
        return enc.finish();
    }

    DocumentFlags C4ConnectedClientImpl::convertDocumentFlags(C4DocumentFlags flags) {
        DocumentFlags docFlags = {};
        if (flags & kRevDeleted)        docFlags |= DocumentFlags::kDeleted;
        if (flags & kRevHasAttachments) docFlags |= DocumentFlags::kHasAttachments;
        return docFlags;
    }

    alloc_slice C4ConnectedClientImpl::generateRevID(Dict body, revid parentRevID, DocumentFlags flags) {
        // Get SHA-1 digest of (length-prefixed) parent rev ID, deletion flag, and JSON:
        alloc_slice json = FLValue_ToJSONX(body, false, true);
        parentRevID.setSize(min(parentRevID.size, size_t(255)));
        uint8_t revLen = (uint8_t)parentRevID.size;
        uint8_t delByte = (flags & DocumentFlags::kDeleted) != 0;
        SHA1 digest = (SHA1Builder() << revLen << parentRevID << delByte << json).finish();
        unsigned generation = parentRevID ? parentRevID.generation() + 1 : 1;
        return alloc_slice(revidBuffer(generation, slice(digest)));
    }
}
