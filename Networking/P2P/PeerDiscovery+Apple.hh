//
// Created by Jens Alfke on 2/3/25.
//

#pragma once
#include "c4PeerDiscovery.hh"
#include "Base.hh"

#ifdef __APPLE__

namespace litecore::p2p {

    static constexpr string_view kDefaultServiceType = "_cblitep2p._tcp";

    void InitializeBonjourProvider(string_view serviceType = kDefaultServiceType);

}

#endif //__APPLE__
