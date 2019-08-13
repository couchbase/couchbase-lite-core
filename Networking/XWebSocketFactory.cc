//
// XWebSocketFactory.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "XWebSocketFactory.hh"
#include "XWebSocket.hh"
#include <mutex>

using namespace std;
using namespace fleece;
using namespace litecore::websocket;

#if 0
static inline XWebSocket* internal(C4Socket *sock) {
    return ((XWebSocket*)sock->nativeHandle);
}


static void sock_open(C4Socket *sock, const C4Address *c4To, FLSlice optionsFleece, void*) {
    auto self = new XWebSocket(sock, *c4To, AllocedDict((slice)optionsFleece));
    self->open();
}


static void sock_write(C4Socket *sock, C4SliceResult allocatedData) {
    if (internal(sock))
        internal(sock)->sendBytes(alloc_slice(move(allocatedData)));
}

static void sock_completedReceive(C4Socket *sock, size_t byteCount) {
    if (internal(sock))
        internal(sock)->completedReceive(byteCount);
}


static void sock_close(C4Socket *sock) {
    if (internal(sock))
        internal(sock)->close(status, message);
}


static void sock_dispose(C4Socket *sock) {
    if (internal(sock))
        internal(sock)->setC4Socket(nullptr);
}


#pragma mark - C4 SOCKET FACTORY:


const C4SocketFactory C4XWebSocketFactory {
    kC4WebSocketClientFraming,
    nullptr, // .context (unused)
    &sock_open,
    &sock_write,
    &sock_completedReceive,
    &sock_close,
    nullptr, // .requestClose (will not be called since I do no framing)
    &sock_dispose
};


void RegisterXWebSocketFactory() {
    static std::once_flag once;
    std::call_once(once, [] {
        c4socket_registerFactory(C4XWebSocketFactory);
    });
}
#endif
