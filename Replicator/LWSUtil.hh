//
// LWSUtil.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "c4Base.h"
#include "libwebsockets.h"
#include "fleece/slice.hh"
#include <utility>

namespace litecore { namespace websocket { namespace LWS {

    // Utility functions used by LWSWebSocket:

    bool addRequestHeader(lws *_client, uint8_t* *dst, uint8_t *end,
                          const char *header, fleece::slice value);

    std::pair<int,std::string> decodeHTTPStatus(lws *client);

    fleece::alloc_slice encodeHTTPHeaders(lws *client);

    C4Error getConnectionError(lws *client, fleece::slice lwsErrorMessage);

    fleece::alloc_slice getCertPublicKey(fleece::slice certPEM);
    fleece::alloc_slice getPeerCertPublicKey(lws *client);

} } }
