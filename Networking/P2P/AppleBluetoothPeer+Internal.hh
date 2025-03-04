//
// AppleBluetoothPeer+Internal.hh
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
#include "c4Socket.hh"
#include "fleece/RefCounted.hh"
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class CBL2CAPChannel;

namespace litecore::p2p {
    static constexpr fleece::slice kBTURLScheme = "l2cap";

    extern C4SocketFactory BTSocketFactory;

    /// Opens a Bluetooth L2CAP channel to a peer, asynchronously.
    void OpenBTChannel(C4Peer*, void (^onComplete)(CBL2CAPChannel* _Nullable, C4Error));

    /// Creates a C4Socket from an open incoming Bluetooth L2CAP connection.
    fleece::Retained<C4Socket> BTSocketFromL2CAPChannel(CBL2CAPChannel* channel, bool incoming);

}  // namespace litecore::p2p

NS_ASSUME_NONNULL_END
