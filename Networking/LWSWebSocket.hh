//
// LWSWebSocket.hh
//
// Copyright (c) 2019 Couchbase, Inc All rights reserved.
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

#pragma once
#include "LWSProtocol.hh"
#include "c4Socket.h"
#include "Address.hh"
#include "Error.hh"
#include "fleece/Fleece.hh"
#include <deque>

namespace litecore { namespace crypto {
    class Cert;
}}

namespace litecore { namespace net {
    class LWSServer;

    /** Abstract superclass of WebSocket connections. */
    class LWSWebSocket : public LWSProtocol {
    public:

        // C4SocketFactory callbacks:
        static void sock_write(C4Socket *sock, C4SliceResult allocatedData);
        static void sock_completedReceive(C4Socket *sock, size_t byteCount);
        static void sock_requestClose(C4Socket *sock, int status, C4String message);
        static void sock_dispose(C4Socket *sock);

    protected:
        LWSWebSocket(lws *client, C4Socket* s);

        void setC4Socket(C4Socket* s);
        void onEvent(lws *wsi, int reason, void *user, void *in, size_t len) override;
        void write(const fleece::alloc_slice &message);
        void requestClose(int status, fleece::slice message);
        void completedReceive(size_t byteCount);
        void _sendFrame(uint8_t opcode, int status, fleece::slice body);
        void onWriteable();
        void onReceivedMessage(fleece::slice data);
        void onCloseRequest(fleece::slice body);
        void onConnectionError(C4Error error) override;
        virtual void onDestroy() override;
        void onClosed();
        void closeC4Socket(C4ErrorDomain domain, int code, C4String message);
        void closeC4Socket(C4Error status);

    protected:
        C4Socket*   _c4socket {nullptr};        // The C4Socket I support
    private:
        ssize_t     _unreadBytes {0};           // # bytes received but not yet handled by repl
        bool        _readsThrottled {false};    // True if libwebsocket flow control stopping reads
        std::deque<fleece::alloc_slice> _outbox;// Messages waiting to be sent [w/padding]
        fleece::alloc_slice _incomingMessage;   // Buffer for assembling fragmented messages
        size_t      _incomingMessageLength {0}; // Current length of message in _incomingMessage
        bool        _sentCloseFrame {false};    // Did I send a CLOSE message yet?
    };



    /** A client-side WebSocket connection. */
    class LWSClientWebSocket : public LWSWebSocket {
    public:
        LWSClientWebSocket(C4Socket *socket,
                           const C4Address &to,
                           const fleece::AllocedDict &options);

        static void sock_open(C4Socket *sock, const C4Address *c4To, FLSlice optionsFleece, void*);

    protected:
        void onEvent(lws *wsi, int reason, void *user, void *in, size_t len) override;
        virtual const char *className() const noexcept override      {return "LWSClientWebSocket";}
    private:
        void open();
        bool onSendCustomHeaders(void *in, size_t len);
        void onConnected();
        void gotResponse();
        void onConnectionError(C4Error error) override;

        litecore::repl::Address _address;       // Address to connect to
        fleece::AllocedDict _options;           // Replicator options
    };



    /** A server-side WebSocket connection (incoming from a peer.) */
    class LWSServerWebSocket : public LWSWebSocket {
    public:
        LWSServerWebSocket(lws* NONNULL, LWSServer* NONNULL);
        ~LWSServerWebSocket();

        C4Socket* c4Socket() const                                  {return _c4socket;}

        void upgraded();
        void canceled();

    protected:
        virtual const char *className() const noexcept override     {return "LWSServerWebSocket";}

    private:
        void createC4Socket();
    };

} }
