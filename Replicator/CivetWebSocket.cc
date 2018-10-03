//
// CivetWebSocket.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "fleece/Fleece.hh"
#include "fleece/slice.hh"
#include "CivetWebSocket.hh"
#include "c4Replicator.h"
#include "Address.hh"
#include "Actor.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "civetweb.h"
#include <exception>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _MSC_VER
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace std;
using namespace fleece;

#undef Log
#undef LogDebug
#define Log(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogInfo, "CivetWebSocket: " MSG, ##__VA_ARGS__)
#define LogDebug(MSG, ...)  C4LogToAt(kC4WebSocketLog, kC4LogDebug, "CivetWebSocket: " MSG, ##__VA_ARGS__)



namespace litecore { namespace websocket {


    static int connectHandler(const struct mg_connection*, void *userData);
    static int dataHandler(struct mg_connection*, int header, char *message, size_t len, void*);
    static void closeHandler(const struct mg_connection *, void*);


    static int toErrno(const struct mg_error& error) {
#ifdef _MSC_VER
        switch(error.code) {
            case WSAECONNREFUSED:   return ECONNREFUSED;
            case WSAENETRESET:      return ENETRESET;
            case WSAECONNABORTED:   return ECONNABORTED;
            case WSAECONNRESET:     return ECONNRESET;
            case WSAETIMEDOUT:      return ETIMEDOUT;
            case WSAENETDOWN:       return ENETDOWN;
            case WSAENETUNREACH:    return ENETUNREACH;
            case WSAENOTCONN:       return ENOTCONN;
            case WSAEHOSTDOWN:      return 64;
            case WSAEHOSTUNREACH:   return EHOSTUNREACH;
        }
#endif
        return error.code;
    }


    static C4Error toC4Error(const mg_error &civetErr) {
        C4ErrorDomain domain;
        int code;
        if (civetErr.code >= MG_ERR_HTTP_STATUS_BASE) {
            domain = WebSocketDomain;
            code = civetErr.code - MG_ERR_HTTP_STATUS_BASE;
        } else if (civetErr.code >= MG_ERR_CIVETWEB_BASE) {
            domain = NetworkDomain;
            switch (civetErr.code) {
                case MG_ERR_INVALID_CERT:
                    code = kC4NetErrTLSClientCertRejected;
                    break;
                case MG_ERR_HOST_NOT_FOUND:
                    code = kC4NetErrUnknownHost;
                    break;
                case MG_ERR_DNS_FAILURE:
                    code = kC4NetErrDNSFailure;
                    break;
                default:
                    C4Warn("CivetWebSocket: No C4Error for CivetWeb status %d", civetErr.code);
                    domain = LiteCoreDomain;
                    code = kC4ErrorUnexpectedError;
                    break;
            }
        } else {
            domain = POSIXDomain;
            code = toErrno(civetErr);
        }
        return c4error_make(domain, code, slice(civetErr.buffer));
    }


#pragma mark - WEBSOCKET CLASS


    class CivetWebSocket : public actor::Actor {
    public:

        // Client-side constructor
        CivetWebSocket(C4Socket *socket,
                       const C4Address &to,
                       const AllocedDict &options)
        :Actor()
        ,_c4socket(socket)
        ,_address(to)
        ,_options(options)
        {
            mg_init_library(0);
        }

        ~CivetWebSocket() {
            DebugAssert(!_connection);
            mg_exit_library();
            logStats();
        }

        void open() {
            enqueue(&CivetWebSocket::_open);
        }

        void onConnected(const mg_connection *connection) {
            c4socket_gotHTTPResponse(_c4socket,
                                     getConnectStatus(connection),
                                     getConnectHeaders(connection));
        }

        void completedReceive(size_t byteCount) {
            enqueue(&CivetWebSocket::_completedReceive, byteCount);
        }

        void send(const alloc_slice &message) {
            enqueue(&CivetWebSocket::_sendMessage, message);
        }

        void onMessage(int headerByte, const alloc_slice &data) {
            enqueue(&CivetWebSocket::_onMessage, headerByte, data);
        }

        void close(int status, const alloc_slice &message) {
            enqueue(&CivetWebSocket::_close, status, message);
        }

        void onClosed() {
            enqueue(&CivetWebSocket::_onClosed);
        }

    private:

        void _open() {
            Assert(!_connection);
            Log("CivetWebSocket connecting to <%.*s>...", SPLAT(_address.url()));

            stringstream extraHeaders;
            for (Dict::iterator header(_options[kC4ReplicatorOptionExtraHeaders].asDict());
                     header; ++header) {
                extraHeaders << header.keyString() << ": " << header.value().asString() << "\r\n";
            }
            slice cookies = _options[kC4ReplicatorOptionCookies].asString();
            if (cookies)
                extraHeaders << "Cookie: " << cookies << "\r\n";

            slice protocols = _options[kC4SocketOptionWSProtocols].asString();
            if (protocols)
                extraHeaders << "Sec-WebSocket-Protocol: " << protocols << "\r\n";

            bool useSSL = _address.isSecure();

            char errorStr[256];
            mg_error civetErr {errorStr, sizeof(errorStr), 0};
            _connection = mg_connect_websocket_client2(slice(_address.hostname).cString(),
                                                       _address.port,
                                                       useSSL,
                                                       &civetErr,
                                                       slice(_address.path).cString(),
                                                       extraHeaders.str().c_str(),
                                                       &connectHandler, &dataHandler, &closeHandler,
                                                       this);
            if (_connection) {
                retain(this);
                mg_set_user_connection_data(_connection, this);
                c4socket_opened(_c4socket);
            } else {
                c4socket_closed(_c4socket, toC4Error(civetErr));
            }
        }


        static int getConnectStatus(const mg_connection *connection) {
            auto ri = mg_get_request_info(connection);
            return stoi(string(ri->request_uri));
        }


        static alloc_slice getConnectHeaders(const mg_connection *connection) {
            // Headers can appear more than once, so collect them into an array-valued map:
            unordered_map<string, vector<string>> headerMap;
            auto ri = mg_get_request_info(connection);
            for (int i = 0; i < ri->num_headers; i++)
                headerMap[ri->http_headers[i].name].emplace_back(ri->http_headers[i].value);

            // Now encode as a Fleece dict, where values are strings or arrays of strings:
            Encoder enc;
            enc.beginDict(headerMap.size());
            for (auto i = headerMap.begin(); i != headerMap.end(); ++i) {
                enc.writeKey(slice(i->first));
                if (i->second.size() == 1)
                    enc.writeString(i->second[0]);
                else {
                    enc.beginArray();
                    for (string &value : i->second)
                        enc.writeString(value);
                    enc.endArray();
                }
            }
            enc.endDict();
            return enc.finish();
        }


        bool _sendFrame(int opcode, alloc_slice body) {
            if (!_connection) return false;
            int ok = mg_websocket_client_write(_connection, opcode, (const char*)body.buf, body.size);
            if (ok < 0) {
                C4Warn("mg_websocket_write failed");
                //TODO: What should I do on error?
                return false;
            }
            return true;
        }


        void _sendMessage(alloc_slice message) {
            _sendFrame(WEBSOCKET_OPCODE_BINARY, message);
            c4socket_completedWrite(_c4socket, message.size);
        }


        void _onMessage(int headerByte, alloc_slice data) {
            switch (headerByte & 0x0F) {
                case WEBSOCKET_OPCODE_BINARY:
                    _pendingBytes += data.size;
                    LogDebug("RECEIVED:  %6zd bytes  (now %6zd pending)",
                             data.size, ssize_t(_pendingBytes));
                    c4socket_received(_c4socket, data);
                    break;
                case WEBSOCKET_OPCODE_PING:
                    _sendFrame(WEBSOCKET_OPCODE_PONG, data);
                    break;
                case WEBSOCKET_OPCODE_CONNECTION_CLOSE:
                    _onCloseRequest(data);
                    break;
                default:
                    break;
            }
        }


        void _completedReceive(size_t byteCount) {
            _pendingBytes -= byteCount;
            LogDebug("COMPLETED: %6zd bytes  (now %6zd pending)",
                     byteCount, ssize_t(_pendingBytes));
            // TODO: flow control (I don't think CivetWeb supports it...)
        }


        void _close(int status, alloc_slice message) {
            if (_sentCloseFrame)
                return;
            LogDebug("Closing with WebSocket status %d '%.*s'", status, SPLAT(message));
            alloc_slice body(2 + message.size);
            *(uint16_t*)body.buf = htons((uint16_t)status);
            memcpy((void*)&body[2], message.buf, message.size);
            _sendCloseFrame(body);
        }

        void _onCloseRequest(alloc_slice body) {
            // https://tools.ietf.org/html/rfc6455#section-7
            LogDebug("Received close request");
            _rcvdCloseFrame = true;
            if (!_sentCloseFrame) {
                // Peer initiated close, so get its reason:
                if (body.size >= 2) {
                    slice in = body;
                    int status = ntohs(*(uint16_t*)in.read(2).buf);
                    _closeStatus = c4error_make(WebSocketDomain, status, in);
                } else {
                    _closeStatus = c4error_make(WebSocketDomain, kWebSocketCloseNoCode, nullslice);
                }
                // Echo back peer's close request (synchronously):
                //TODO: Give the delegate a chance to delay this
                _sendCloseFrame(body);
            }
            mg_close_connection(_connection);
            _onClosed();
        }


        void _sendCloseFrame(alloc_slice body) {
            if (_sendFrame(WEBSOCKET_OPCODE_CONNECTION_CLOSE, body))
                _sentCloseFrame = true;
        }


        void _onClosed() {
            Log("Connection closed");
            if (!_connection)
                return;
            _connection = nullptr;
            if (!_rcvdCloseFrame) {
                _closeStatus = c4error_make(WebSocketDomain, kWebSocketCloseAbnormal,
                                            "Connection closed unexpectedly"_sl);
            }
            c4socket_closed(_c4socket, _closeStatus);
            release(this); // balances retain() in _open()
        }


        litecore::repl::Address _address;
        C4Socket* _c4socket;
        AllocedDict _options;
        struct mg_connection *_connection {nullptr};
        alloc_slice _responseHeaders;
        C4Error _closeStatus {};
        bool _accepted {false};
        bool _sentCloseFrame {false};
        bool _rcvdCloseFrame {false};
        ssize_t _pendingBytes {0};
    };


#pragma mark - CIVETWEB CONNECTION CALLBACKS:


    static int connectHandler(const struct mg_connection *connection, void *userData) {
        ((CivetWebSocket*)userData)->onConnected(connection);
        return 0;
    }


    static int dataHandler(struct mg_connection *connection,
                           int header, char *message, size_t messageLen, void *userData)
    {
        ((CivetWebSocket*)userData)->onMessage(header & 0x0F, alloc_slice(message, messageLen));
        return 1;
    }


    static void closeHandler(const struct mg_connection *connection, void *userData) {
        ((CivetWebSocket*)userData)->onClosed();
    }


#pragma mark - C4 SOCKET FACTORY:


    static inline CivetWebSocket* internal(C4Socket *sock) {
        return ((CivetWebSocket*)sock->nativeHandle);
    }


    static void sock_open(C4Socket *sock, const C4Address *c4To, FLSlice optionsFleece, void*) {
        auto self = new CivetWebSocket(sock, *c4To, AllocedDict((slice)optionsFleece));
        sock->nativeHandle = self;
        retain(self);  // Makes nativeHandle a strong ref; balanced by release in sock_dispose
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
            internal(sock)->close(status, alloc_slice(message));
    }


    static void sock_dispose(C4Socket *sock) {
        release(internal(sock));        // balances retain in sock_open
        sock->nativeHandle = nullptr;
    }


} }

using namespace litecore::websocket;


const C4SocketFactory C4CivetWebSocketFactory {
    kC4NoFraming,
    nullptr,
    &sock_open,
    &sock_write,
    &sock_completedReceive,
    nullptr,
    &sock_requestClose,
    &sock_dispose
};
