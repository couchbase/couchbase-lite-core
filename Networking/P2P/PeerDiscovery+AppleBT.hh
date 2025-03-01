//
// PeerDiscovery+AppleBT.hh
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
#include "c4PeerDiscovery.hh"
#include "Base.hh"

#ifdef __APPLE__

namespace litecore::p2p {

    void InitializeBluetoothProvider(string_view serviceType);

}  // namespace litecore::p2p

#endif  //__APPLE__
