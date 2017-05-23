//
//  c4ListenerInternal.hh
//  LiteCore
//
//  Created by Jens Alfke on 5/23/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4Listener.h"

namespace litecore { namespace REST {
    class Listener;

    
    extern C4LogDomain RESTLog;


    extern const C4ListenerAPIs kListenerAPIs;
    Listener* NewListener(const C4ListenerConfig *config);
    
    
} }
