//
// Created by Jens Alfke on 2/3/25.
//

#pragma once
#include "c4PeerDiscovery.hh"
#include "Base.hh"

#ifdef __APPLE__

namespace litecore::p2p {

    void InitializeBonjourProvider(string_view serviceType);

    fleece::alloc_slice EncodeMetadataAsTXT(C4Peer::Metadata const&, int* outError = nullptr);
    C4Peer::Metadata    DecodeTXTToMetadata(slice txtRecord);
}  // namespace litecore::p2p

#endif  //__APPLE__
