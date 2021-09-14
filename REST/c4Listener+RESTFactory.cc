//
// c4Listener+Factory.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4ListenerInternal.hh"
#include "RESTListener.hh"

#ifndef COUCHBASE_ENTERPRISE

namespace litecore { namespace REST {

    const C4ListenerAPIs kListenerAPIs = kC4RESTAPI;

    Retained<Listener> NewListener(const C4ListenerConfig *config) {
        if (config->apis == kC4RESTAPI)
            return new RESTListener(*config);
        else
            return nullptr;
    }

} }

#endif
