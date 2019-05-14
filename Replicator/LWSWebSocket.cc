//
// LWSWebSocket.cc
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

#include "LWSWebSocket.hh"
#include "c4Replicator.h"
#include "Address.hh"
#include "Error.hh"
#include "RefCounted.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include "fleece/slice.hh"

#include "libwebsockets.h"

#include <deque>
#include <mutex>
#include <thread>

using namespace std;
using namespace fleece;

#undef Log
#undef LogDebug
#define Log(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogInfo, "LWSWebSocket: " MSG, ##__VA_ARGS__)
#define LogDebug(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogDebug, "LWSWebSocket: " MSG, ##__VA_ARGS__)


#define LWS_WRITE_CLOSE lws_write_protocol(4)


namespace litecore { namespace websocket {


    // Max number of bytes read that haven't been handled by the replicator yet.
    // Beyond this point, we turn on backpressure (flow-control) in libwebsockets
    // so it stops reqding the socket.
    static constexpr size_t kMaxUnreadBytes = 100 * 1024;


#pragma mark - CONTEXT:


    /** Singleton that manages the libwebsocket context and event thread. */
    class LWSContext {
    public:
        static void initialize(const struct lws_protocols protocols[]) {
            if (!instance)
                instance = new LWSContext(protocols);
        }

        static LWSContext* instance;

        LWSContext(const struct lws_protocols protocols[]) {
            // Configure libwebsocket logging:
            int flags = LLL_ERR  | LLL_WARN | LLL_NOTICE | LLL_INFO;
#if DEBUG
            flags |= LLL_DEBUG;
#endif
            lws_set_log_level(flags, &logCallback);

            struct lws_context_creation_info info = {};
            info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
            info.port = CONTEXT_PORT_NO_LISTEN; /* we do not run any server */
            info.protocols = protocols;
            _context = lws_create_context(&info);
            if (!_context)
                return;

            _thread.reset( new thread([&]() {
                while (true) {
                    lws_service(_context, 999999);  // TODO: How/when to stop this
                }
            }));
        }

        bool isOpen()                               {return _context != nullptr;}

        struct lws_context* context() const         {return _context;}

        ~LWSContext() {
            // FIX: How to stop the thread?
            if (_context)
                lws_context_destroy(_context);
        }

    private:
        static void logCallback(int level, const char *message) {
            slice msg(message);
            if (msg.size > 0 && msg[msg.size-1] == '\n')
                msg.setSize(msg.size-1);
            if (msg.size == 0)
                return;
            C4LogLevel c4level;
            switch(level) {
                case LLL_ERR:    c4level = kC4LogError; break;
                case LLL_WARN:   c4level = kC4LogWarning; break;
                case LLL_NOTICE: c4level = kC4LogInfo; break;
                case LLL_INFO:   c4level = kC4LogInfo; break;
                default:         c4level = kC4LogDebug; break;
            }
            C4LogToAt(kC4WebSocketLog, c4level, "libwebsocket: %.*s", SPLAT(msg));
        }

        struct lws_context* _context {nullptr};
        unique_ptr<thread> _thread;
    };


    LWSContext* LWSContext::instance = nullptr;


#pragma mark - WEBSOCKET CLASS


    class LWSWebSocket : public RefCounted {
    public:

        // Client-side constructor
        LWSWebSocket(C4Socket *socket,
                       const C4Address &to,
                       const AllocedDict &options)
        :_c4socket(socket)
        ,_address(to)
        ,_options(options)
        { }


        ~LWSWebSocket() {
            DebugAssert(!_client);
        }

        //// Called by the C4Socket, via the C4SocketFactory callbacks below:

        void open() {
            Assert(!_client);
            Log("LWSWebSocket connecting to <%.*s>...", SPLAT(_address.url()));

            // Create LWS context:
            LWSContext::initialize(kProtocols);

            // Create LWS client and connect:
            string hostname(slice(_address.hostname));
            string path(slice(_address.path));

            struct lws_client_connect_info i = {};
            i.context = LWSContext::instance->context();
            i.port = _address.port;
            i.address = hostname.c_str();
            i.path = path.c_str();
            i.host = i.address;
            i.origin = i.address;
            if (_address.isSecure())
                i.ssl_connection = LCCSCF_USE_SSL;
            i.protocol = kProtocols[0].name;
            i.pwsi = &_client;                  // LWS will fill this in when it connects
            i.opaque_user_data = this;
            (void) lws_client_connect_via_info(&i);
        }


        void completedReceive(size_t byteCount) {
            synchronized([&]{
                _unreadBytes -= byteCount;
                LogDebug("Completed receive of %6zd bytes  (now %6zd pending)",
                         byteCount, ssize_t(_unreadBytes));
                if (_readsThrottled && _unreadBytes <= (kMaxUnreadBytes / 2)) {
                    Log("Un-throttling input (caught up)");
                    _readsThrottled = false;
                    lws_rx_flow_control(_client, 1 | LWS_RXFLOW_REASON_FLAG_PROCESS_NOW);
                }
            });
        }


        void send(const alloc_slice &message) {
            LogDebug("Queuing send of %zu byte message", message.size);
            _sendFrame(LWS_WRITE_BINARY, LWS_CLOSE_STATUS_NOSTATUS, message);
        }

        void requestClose(int status, alloc_slice message) {
            Log("Closing with WebSocket status %d '%.*s'", status, SPLAT(message));
            _sendFrame(LWS_WRITE_CLOSE, (lws_close_status)status, message);
        }

    private:

        void _sendFrame(enum lws_write_protocol opcode, lws_close_status status, slice body) {
            alloc_slice frame(LWS_PRE + body.size);
            memcpy((void*)&frame[LWS_PRE], body.buf, body.size);
            (uint8_t&)frame[0] = opcode;
            memcpy((void*)&frame[1], &status, sizeof(status));

            synchronized([&]{
                _outbox.push_back(frame);
                if (_outbox.size() == 1)
                    lws_callback_on_writable(_client); // Will trigger LWS_CALLBACK_CLIENT_WRITEABLE
            });
        }


#pragma mark - LIBWEBSOCKETS CALLBACK:


        int callback(struct lws *wsi, enum lws_callback_reasons reason, void *user,
                     void *in, size_t len)
        {
            switch (reason) {
                    // Client lifecycle:
                case LWS_CALLBACK_WSI_CREATE:
                    LogDebug("**** LWS_CALLBACK_WSI_CREATE");
                  if (!_client)
                      _client = wsi;
                    retain(this);
                    break;
                case LWS_CALLBACK_WSI_DESTROY:
                    LogDebug("**** LWS_CALLBACK_WSI_DESTROY");
                    _client = nullptr;
                    release(this);
                    break;

                    // Connecting:
                case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                    LogDebug("**** LWS_CALLBACK_CLIENT_CONNECTION_ERROR");
                    onConnectionError(slice(in, len));
                    break;
                case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
                    LogDebug("**** LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER");
                    if (!onSendCustomHeaders(in, len))
                        return -1;
                    break;
                case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
                    LogDebug("**** LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH");
                    onConnected();
                    break;

                    // Read/write:
                case LWS_CALLBACK_CLIENT_WRITEABLE:
                    LogDebug("**** LWS_CALLBACK_CLIENT_WRITEABLE");
                    if (!onWriteable())
                        return -1;
                    break;
                case LWS_CALLBACK_CLIENT_RECEIVE:
                    onReceivedMessage(slice(in, len));
                    break;

                    // Close:
                case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
                    // "If you return 0 lws will echo the close and then close the
                    // connection.  If you return nonzero lws will just close the
                    // connection."
                    LogDebug("**** LWS_CALLBACK_WS_PEER_INITIATED_CLOSE");
                    return onCloseRequest(slice(in, len)) ? 0 : 1;
                case LWS_CALLBACK_CLIENT_CLOSED:
                    LogDebug("**** LWS_CALLBACK_CLIENT_CLOSED");
                    onClosed();
                    break;
                default:
                    if (reason < 31 || reason > 36)
                        LogDebug("**** CALLBACK #%d", reason);
                    break;
            }

            return lws_callback_http_dummy(wsi, reason, user, in, len);
        }


        static int callback_blip(struct lws *wsi, enum lws_callback_reasons reason,
                                 void *user, void *in, size_t len)
        {
            auto self = (LWSWebSocket*) lws_get_opaque_user_data(wsi);
            if (self)
                return self->callback(wsi, reason, user, in, len);
            else
                return lws_callback_http_dummy(wsi, reason, user, in, len);
        }


        constexpr static const struct lws_protocols kProtocols[] = {
            { "BLIP_3+CBMobile_2", callback_blip, 0, 0},
            { NULL, NULL, 0, 0 }
        };


#pragma mark - HANDLERS:


        // Returns false if libwebsocket wouldn't let us write all the headers
        bool onSendCustomHeaders(void *in, size_t len) {
            // "in is a char **, it's pointing to a char * which holds the
            // next location in the header buffer where you can add
            // headers, and len is the remaining space in the header buffer"
            auto dst = (uint8_t**)in;
            uint8_t *end = *dst + len;

            // Subroutine to append a header to `dst`:
            auto addHeader = [&](const char *header, slice value) -> bool {
                int i = lws_add_http_header_by_name(_client, (const uint8_t*)header,
                                                    (const uint8_t*)value.buf, int(value.size),
                                                    dst, end);
                if (i != 0) {
                    C4LogToAt(kC4WebSocketLog, kC4LogError,
                              "libwebsockets wouldn't let me add enough HTTP headers");
                    return false;
                }
                LogDebug("Added header:  %s %.*s", header, SPLAT(value));
                return true;
            };

            // Add auth header:
            Dict auth = _options[kC4ReplicatorOptionAuthentication].asDict();
            if (auth) {
                slice authType = auth[kC4ReplicatorAuthType].asString();
                if (authType == slice(kC4AuthTypeBasic)) {
                    auto user = auth[kC4ReplicatorAuthUserName].asString();
                    auto pass = auth[kC4ReplicatorAuthPassword].asString();
                    string cred = slice(format("%.*s:%.*s", SPLAT(user), SPLAT(pass))).base64String();
                    if (!addHeader("Authorization:", slice("Basic " + cred)))
                        return false;
                } else {
                    c4socket_closed(_c4socket, c4error_make(LiteCoreDomain, kC4ErrorInvalidParameter,
                                                            "Unsupported auth type"_sl));
                    return false;
                }
            }

            // Add cookie header:
            slice cookies = _options[kC4ReplicatorOptionCookies].asString();
            if (cookies) {
                if (!addHeader("Cookie:", cookies))
                    return false;
            }

            // Add other custom headers:
            Dict::iterator header(_options[kC4ReplicatorOptionExtraHeaders].asDict());
            for (; header; ++header) {
                string headerStr = string(header.keyString()) + ':';
                if (!addHeader(headerStr.c_str(), header.value().asString()))
                    return false;
            }
            return true;
        }


        void onConnected() {
            Log("Client established!");
            c4socket_gotHTTPResponse(_c4socket, decodeHTTPStatus(), decodeHTTPHeaders());
            c4socket_opened(_c4socket);
        }


        bool onWriteable() {
            // Pop first message from outbox queue:
            alloc_slice msg;
            bool more;
            synchronized([&]{
                if (!_outbox.empty()) {
                    msg = _outbox.front();
                    _outbox.pop_front();
                    more = !_outbox.empty();
                }
            });
            if (!msg)
                return true;

            // Write it:
            auto opcode = (enum lws_write_protocol) msg[0];
            slice payload = msg;
            payload.moveStart(LWS_PRE);

            if (opcode != LWS_WRITE_CLOSE) {
                // Regular WebSocket message:
                int m = lws_write(_client, (uint8_t*)payload.buf, payload.size, opcode);
                if (m < payload.size) {
                    Log("ERROR %d writing to ws socket\n", m);
                    return false;
                }

                // Notify C4Socket that it was written:
                c4socket_completedWrite(_c4socket, payload.size);

                // Schedule another onWriteable call if there are more messages:
                if (more) {
                    synchronized([&]{
                        lws_callback_on_writable(_client);
                    });
                }
                return true;

            } else {
                // I'm initiating closing the socket. Set the status/reason to go in the CLOSE msg:
                synchronized([&]{
                    Assert(!_sentCloseFrame);
                    _sentCloseFrame = true;
                });
                lws_close_status status;
                memcpy(&status, &msg[1], sizeof(status));
                LogDebug("Writing CLOSE message, status %d, msg '%.*s'", status, SPLAT(payload));
                lws_close_reason(_client, status, (uint8_t*)payload.buf, payload.size);
                return false;
            }
        }


        void onReceivedMessage(slice data) {
            LogDebug("**** LWS_CALLBACK_CLIENT_RECEIVE  %4zd bytes  (%zd remaining)",
                     data.size, lws_remaining_packet_payload(_client));

            bool final = lws_is_final_fragment(_client);
            if (!final && !_incomingMessage) {
                // Beginning of fragmented message:
                _incomingMessage = alloc_slice(data.size + lws_remaining_packet_payload(_client));
                _incomingMessageLength = 0;
            }

            if (_incomingMessage) {
                Assert(_incomingMessageLength + data.size <= _incomingMessage.size);
                memcpy((void*)&_incomingMessage[_incomingMessageLength], data.buf, data.size);
                _incomingMessageLength += data.size;
                data = _incomingMessage;
            }

            if (final) {
                synchronized([&]{
                    _unreadBytes += data.size;
                    if (!_readsThrottled && _unreadBytes > kMaxUnreadBytes) {
                        Log("Throttling input (receiving too fast)");
                        _readsThrottled = true;
                        lws_rx_flow_control(_client, 0);
                    }
                });
                c4socket_received(_c4socket, data);

                _incomingMessage = nullslice;
            }
        }


        // Peer initiating close. Returns true if I should send back a CLOSE message
        bool onCloseRequest(slice body) {
            // https://tools.ietf.org/html/rfc6455#section-7
            LogDebug("Received close request");
            _rcvdCloseFrame = true;
            bool sendCloseFrame = !_sentCloseFrame;
            _sentCloseFrame = true;
            return sendCloseFrame;
        }


        void onConnectionError(slice errorMessage) {
            static constexpr struct {slice string; C4ErrorDomain domain; int code;} kMessages[] = {
                {"HS: ws upgrade unauthorized"_sl, WebSocketDomain, 401},
                { }
            };

            string statusMessage;
            int status = decodeHTTPStatus(&statusMessage);
            alloc_slice headers = decodeHTTPHeaders();
            if (status || headers)
                c4socket_gotHTTPResponse(_c4socket, status, headers);

            C4Error closeStatus = {};
            if (status >= 300) {
                closeStatus = c4error_make(WebSocketDomain, status, slice(statusMessage));
            } else if (errorMessage) {
                // LWS does not provide any sort of error code, so just look up the string:
                for (int i = 0; kMessages[i].string; ++i) {
                    if (errorMessage == kMessages[i].string) {
                        closeStatus = c4error_make(kMessages[i].domain, kMessages[i].code, errorMessage);
                    }
                }
            } else {
                errorMessage = "unknown error"_sl;
            }

            if (!closeStatus.code)
                closeStatus = c4error_make(NetworkDomain, kC4NetErrUnknown, errorMessage);
            Log("Connection error: %.*s", SPLAT(errorMessage));
            c4socket_closed(_c4socket, closeStatus);
        }


        void onClosed() {
            C4Error closeStatus;
            synchronized([&]() {
                if (_sentCloseFrame) {
                    Log("Connection closed");
                    closeStatus = {WebSocketDomain, kWebSocketCloseNormal};
                } else {
                    Log("Server unexpectedly closed connection");
                    closeStatus = c4error_make(WebSocketDomain, kWebSocketCloseAbnormal,
                                               "Server unexpectedly closed connection"_sl);
                }
            });
            c4socket_closed(_c4socket, closeStatus);
        }


#pragma mark - UTILITIES:


        int decodeHTTPStatus(string *message =nullptr) {
            char buf[32];
            if (lws_hdr_copy(_client, buf, sizeof(buf) - 1, WSI_TOKEN_HTTP) < 0)
                return 0;
            if (message) {
                auto space = strchr(buf, ' ');
                if (space)
                    *message = string(space+1);
            }
            return atoi(buf);
        }


        alloc_slice decodeHTTPHeaders() {
            // libwebsockets makes it kind of a pain to get the HTTP headers...
            Encoder headers;
            headers.beginDict();

            char buf[1024];
            bool any = false;
            for (auto token = WSI_TOKEN_HOST; ; token = (enum lws_token_indexes)(token + 1)) {
                if (token == WSI_TOKEN_HTTP)
                    continue;
                auto headerStr = (const char*) lws_token_to_string(token);
                if (!headerStr)
                    break;
                if (!*headerStr)
                    continue;

                int size = lws_hdr_copy(_client, buf, sizeof(buf), token);
                if (size < 0)
                    Log("Warning: HTTP response header %s is too long", headerStr);
                if (size <= 0)
                    continue;

                char header[32];
                bool caps = true;
                strncpy(header, headerStr, sizeof(header));
                for (char *cp = &header[0]; *cp ; ++cp) {
                    if (*cp == ':') {
                        *cp = '\0';
                        break;
                    } else if (isalpha(*cp)) {
                        if (caps)
                            *cp = (char) toupper(*cp);
                        caps = false;
                    } else {
                        caps = true;
                    }
                }

                //LogDebug("      %s: %.*s", header, size, buf);
                headers.writeKey(slice(header));
                headers.writeString(slice(buf, size));
                any = true;
            }

            headers.endDict();
            if (!any)
                return {};
            return headers.finish();
        }


        template <class BLOCK>
        void synchronized(BLOCK block) {
            lock_guard<mutex> _lock(_mutex);
            block();
        }


        mutex _mutex;                   // For synchronization

        C4Socket* _c4socket;
        litecore::repl::Address _address;
        AllocedDict _options;
        alloc_slice _responseHeaders;

        struct lws* _client {nullptr};

        ssize_t _unreadBytes {0};       // # bytes received but not yet handled by replicator
        bool _readsThrottled {false};   // True if libwebsocket flow control is stopping reads
        deque<alloc_slice> _outbox;     // Messages waiting to be sent [prefixed with padding]

        alloc_slice _incomingMessage;
        size_t _incomingMessageLength {0};

        bool _sentCloseFrame {false};
        bool _rcvdCloseFrame {false};
    };


    const struct lws_protocols LWSWebSocket::kProtocols[2];


#pragma mark - C4 SOCKET FACTORY:


    static inline LWSWebSocket* internal(C4Socket *sock) {
        return ((LWSWebSocket*)sock->nativeHandle);
    }


    static void sock_open(C4Socket *sock, const C4Address *c4To, FLSlice optionsFleece, void*) {
        auto self = new LWSWebSocket(sock, *c4To, AllocedDict((slice)optionsFleece));
        sock->nativeHandle = self;
        retain(self);  // Makes nativeHandle a strong ref; balanced by release in _onClosed
        self->open();
    }


    static void sock_write(C4Socket *sock, C4SliceResult allocatedData) {
        if (internal(sock))
            internal(sock)->send(alloc_slice(move(allocatedData)));
    }

    static void sock_completedReceive(C4Socket *sock, size_t byteCount) {
        if (internal(sock))
            internal(sock)->completedReceive(byteCount);
    }


    static void sock_requestClose(C4Socket *sock, int status, C4String message) {
        if (internal(sock))
            internal(sock)->requestClose(status, alloc_slice(message));
    }


    static void sock_dispose(C4Socket *sock) {
        release(internal(sock));        // balances retain in sock_open
        sock->nativeHandle = nullptr;
    }


} }

using namespace litecore::websocket;


const C4SocketFactory C4LWSWebSocketFactory {
    kC4NoFraming,
    nullptr,
    &sock_open,
    &sock_write,
    &sock_completedReceive,
    nullptr,
    &sock_requestClose,
    &sock_dispose
};


void RegisterC4LWSWebSocketFactory() {
    static std::once_flag once;
    std::call_once(once, [] {
        c4socket_registerFactory(C4LWSWebSocketFactory);
    });
}
