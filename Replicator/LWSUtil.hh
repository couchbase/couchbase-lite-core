//
// LWSUtil.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "libwebsockets.h"

namespace litecore { namespace websocket {

    const char* LWSCallbackName(int /*lws_callback_reasons*/ reason);

} }
