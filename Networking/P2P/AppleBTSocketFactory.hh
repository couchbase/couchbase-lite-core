//
//  AppleBTSocketFactory.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/26/25.
//  Copyright © 2025 Couchbase. All rights reserved.
//

#pragma once
#include "c4SocketTypes.h"
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

#ifdef __OBJC__
@class CBL2CAPChannel;
#endif

namespace litecore::p2p {
    extern C4SocketFactory BTSocketFactory;

#ifdef __OBJC__
    /// Creates a C4Socket from an open incoming Bluetooth L2CAP connection.
    C4Socket* BTSocketFromL2CAPChannel(CBL2CAPChannel* channel, bool incoming);
#endif
}  // namespace litecore::p2p

NS_ASSUME_NONNULL_END
