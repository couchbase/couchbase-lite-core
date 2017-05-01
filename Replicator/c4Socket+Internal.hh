//
//  c4Socket+Internal.hh
//  LiteCore
//
//  Created by Jens Alfke on 3/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4Socket.h"
#include "WebSocketInterface.hh"


namespace litecore { namespace websocket {

    /** A default C4SocketFactory, which will be registered when the first replication starts,
        if the app has not registered its own custom factory yet. */
    extern const C4SocketFactory C4DefaultSocketFactory;

    Provider& DefaultProvider();

} }
