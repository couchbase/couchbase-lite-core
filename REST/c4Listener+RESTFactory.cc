//
//  c4Listener+Factory.cc
//  LiteCore
//
//  Created by Jens Alfke on 5/23/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4ListenerInternal.hh"
#include "RESTListener.hh"

namespace litecore { namespace REST {

    const C4ListenerAPIs kListenerAPIs = kC4RESTAPI;

    Listener* NewListener(const C4ListenerConfig *config) {
        if (config->apis == kC4RESTAPI)
            return new RESTListener(*config);
        else
            return nullptr;
    }

} }
