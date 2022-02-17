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

#include "WebSocketImpl.hh"
#include "WebSocketProtocol.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "Timer.hh"
#include <chrono>
#include <functional>
#include <string>

using namespace std;
using namespace fleece;


#pragma mark - WEBSOCKET:


namespace litecore { namespace websocket {

    LogDomain WSLogDomain("WS", LogLevel::Warning);


    WebSocket::WebSocket(const alloc_slice &a, Role role)
    :_url(a)
    ,_role(role)
    { }


    WebSocket::~WebSocket() =default;


    Delegate& WebSocket::delegate() const {
        DebugAssert(_delegate); return *_delegate;
    }


    void WebSocket::connect(Delegate *delegate) {
        DebugAssert(!_delegate);
        DebugAssert(delegate);
        _delegate = delegate;
        connect();
    }


    const char* CloseStatus::reasonName() const  {
        static const char* kReasonNames[] = {"WebSocket/HTTP status", "errno",
            "Network error", "Exception", "Unknown error"};
        DebugAssert(reason < CloseReason(5));
        return kReasonNames[reason];
    }

} }
