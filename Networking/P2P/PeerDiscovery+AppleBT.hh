//
// Created by Jens Alfke on 2/24/25.
//

#pragma once
#include "c4PeerDiscovery.hh"
#include "Base.hh"

#ifdef __APPLE__

namespace litecore::p2p {

    void InitializeBluetoothProvider(string_view serviceType);

}

#endif  //__APPLE__
