//
// AppleBonjourPeer.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#ifdef __APPLE__
#    include "c4PeerDiscovery.hh"
#    include "fleece/slice.hh"

namespace litecore::p2p {

    /// Must be called once before instantiating C4PeerDiscovery.
    void RegisterBonjourProvider();

    fleece::alloc_slice EncodeMetadataAsTXT(C4Peer::Metadata const&, int* outError = nullptr);
    C4Peer::Metadata    DecodeTXTToMetadata(fleece::slice txtRecord);
}  // namespace litecore::p2p

#endif  //__APPLE__
