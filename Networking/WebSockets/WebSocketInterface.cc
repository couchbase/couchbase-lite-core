//
// WebSocketInterface.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Error.hh"
#include "Logging.hh"
#include "WebSocketInterface.hh"
#include <chrono>
#include <functional>
#include <string>
#include <utility>

using namespace std;
using namespace fleece;


#pragma mark - WEBSOCKET:

namespace litecore::websocket {

    LogDomain WSLogDomain("WS", LogLevel::Warning);

    WebSocket::WebSocket(alloc_slice a, Role role) : _url(std::move(a)), _role(role) {}

    WebSocket::~WebSocket() = default;

    void WebSocket::connect(Retained<WeakHolder<Delegate>> weakDelegate) {
        DebugAssert(!_delegateWeakHolder);
        _delegateWeakHolder = std::move(weakDelegate);
        connect();
    }

    const char* CloseStatus::reasonName() const {
        static const char* kReasonNames[] = {"WebSocket/HTTP status", "errno", "Network error", "Exception",
                                             "Unknown error"};
        DebugAssert(reason < CloseReason(5));
        return kReasonNames[reason];
    }

}  // namespace litecore::websocket
