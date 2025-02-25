//
// Created by Jens Alfke on 2/3/25.
//

#pragma once
#include "c4PeerDiscovery.hh"
#include "Base.hh"

#ifdef __APPLE__

namespace litecore::p2p {

    void InitializeBonjourProvider(string_view serviceType);

}

#endif  //__APPLE__
