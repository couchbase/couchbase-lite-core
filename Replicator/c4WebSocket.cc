//
//  c4Socket.cc
//  LiteCore
//
//  Created by Jens Alfke on 3/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "FleeceCpp.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Socket.h"
#include "c4Socket+Internal.hh"
#include "WebSocketImpl.hh"
#include "WebSocketProtocol.hh"
#include "StringUtil.hh"
#include <atomic>
#include <exception>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;
using namespace litecore::websocket;


namespace litecore { namespace websocket {

    static LogDomain WSLogDomain("WS");

    static constexpr size_t kMaxMessageLength = 1 << 20;

    static constexpr size_t kSendBufferSize = 64 * 1024;

    class NoFrameProviderImpl : public Provider
    {
    public:
        virtual void addProtocol(const std::string &protocol) override {
            _protocols.insert(protocol);
        }

    protected:
        friend class C4SocketImpl;

        virtual void openSocket(WebSocket*) = 0;
        virtual void closeSocket(WebSocket*) = 0;
        virtual void sendBytes(WebSocket*, fleece::alloc_slice) = 0;
        virtual void receiveComplete(WebSocket*, size_t byteCount) = 0;

    private:
        std::set<std::string> _protocols;
    };

    class C4SocketImpl : public WebSocket, public C4Socket, Logging {
    public:
        C4SocketImpl(Provider &provider, const Address &address)
        :WebSocket(provider, address),
        Logging(WSLogDomain)
        {
            nativeHandle = nullptr;
        }

        virtual bool send(fleece::slice message, bool binary = true) override {
            return sendOp(message, binary ? uWS::BINARY : uWS::TEXT);
        }

        virtual void close(int status = 1000, fleece::slice message = fleece::nullslice) override {
            log("Requesting close with status=%d, message='%.*s'", status, SPLAT(message));
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_closeSent || _closeReceived)
                    return;
                _closeSent = true;
                _closeMessage = alloc_slice(2 + message.size);
                auto size = formatClosePayload((char*)_closeMessage.buf,
                    (uint16_t)status,
                    (char*)message.buf, message.size);
                _closeMessage.shorten(size);
            }
            sendOp(_closeMessage, uWS::CLOSE);
        }

        void onConnect() {
            _timeConnected.start();
            delegate().onWebSocketConnect();
        }

        void onWriteComplete(size_t size) {
            bool notify;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _bytesSent += size;
                notify = (_bufferedBytes > kSendBufferSize);
                _bufferedBytes -= size;
                if (_bufferedBytes > kSendBufferSize)
                    notify = false;
            }

            if (notify) {
                delegate().onWebSocketWriteable();
            }
        }

        void onReceive(slice data) {
            {
                // Lock the mutex; this protects all methods (below) involved in receiving,
                // since they're called from this one.
                std::lock_guard<std::mutex> lock(_mutex);

                _bytesReceived += data.size;
                receivedMessage(uWS::BINARY, alloc_slice(data));

                // ... this will call handleFragment(), below
            }
            provider().receiveComplete(this, data.size);
        }

        void onClose(int err_no) {
            CloseStatus status = {};
            {
                std::lock_guard<std::mutex> lock(_mutex);

                _timeConnected.stop();
                double t = _timeConnected.elapsed();
                log("sent %llu bytes, rcvd %llu, in %.3f sec (%.0f/sec, %.0f/sec)",
                    _bytesSent, _bytesReceived, t,
                    _bytesSent / t, _bytesReceived / t);

                if (err_no == 0) {
                    status.reason = kWebSocketClose;
                    if (!_closeSent || !_closeReceived)
                        status.code = kCodeAbnormal;
                    else if (!_closeMessage)
                        status.code = kCodeNormal;
                    else {
                        auto msg = parseClosePayload((char*)_closeMessage.buf,
                            _closeMessage.size);
                        status.code = msg.code ? msg.code : kCodeStatusCodeExpected;
                        status.message = slice(msg.message, msg.length);
                    }
                }
                else {
                    status.reason = kPOSIXError;
                    status.code = err_no;
                }
            }
            delegate().onWebSocketClose(status);
        }

    protected:
        virtual void connect() override {
            provider().openSocket(this);
        }

    private:
        bool sendOp(fleece::slice message, int opcode) {
            bool writeable;
            alloc_slice frame;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_closeSent && opcode != uWS::CLOSE)
                    return false;

                frame.resize(message.size + 1);
                ((char *)frame.buf)[0] = (char)opcode;
                memcpy((char *)frame.buf + 1, message.buf, message.size);
                _bufferedBytes += frame.size;
                writeable = (_bufferedBytes <= kSendBufferSize);
            }

            provider().sendBytes(this, frame);
            return writeable;
        }

        bool receivedClose(slice message) {
            if (_closeReceived)
                return false;
            _closeReceived = true;
            if (_closeSent) {
                // I initiated the close; the peer has confirmed, so disconnect the socket now:
                log("Close confirmed by peer; disconnecting socket now");
                provider().closeSocket(this);
            }
            else {
                // Peer is initiating a close. Save its message and echo it:
                if (willLog()) {
                    auto close = ClientProtocol::parseClosePayload((char*)message.buf, message.size);
                    log("Client is requesting close (%d '%.*s'); echoing it",
                        close.code, (int)close.length, close.message);
                }
                _closeMessage = message;
                sendOp(message, uWS::CLOSE);
            }
            return true;
        }

        bool receivedMessage(int opCode, alloc_slice message) {
            switch (opCode) {
            case uWS::TEXT:
                if (!ClientProtocol::isValidUtf8((unsigned char*)message.buf,
                    message.size))
                    return false;
                // fall through:
            case uWS::BINARY:
                delegate().onWebSocketMessage(message, (opCode == uWS::BINARY));
                return true;
            case uWS::CLOSE:
                return receivedClose(message);
            case uWS::PING:
                send(message, uWS::PONG);
                return true;
            case uWS::PONG:
                //receivedPong(message);
                return true;
            default:
                return false;
            }
        }

        struct CloseFrame {
            uint16_t code;
            char *message;
            size_t length;
        };

        using ClientProtocol = uWS::WebSocketProtocol<false>;
        static inline CloseFrame parseClosePayload(char *src, size_t length) {
            CloseFrame cf = {};
            if (length >= 2) {
                memcpy(&cf.code, src, 2);
                if (cf.code < 1000 || cf.code > 4999 || (cf.code > 1011 && cf.code < 4000) ||
                    (cf.code >= 1004 && cf.code <= 1006) || !ClientProtocol::isValidUtf8((unsigned char *)cf.message, cf.length)) {
                    return{};
                }
            }
            return cf;
        }

        static inline size_t formatClosePayload(char *dst, uint16_t code, const char *message, size_t length) {
            if (code) {
                memcpy(dst, &code, 2);
                memcpy(dst + 2, message, length);
                return length + 2;
            }
            return 0;
        }

        NoFrameProviderImpl& provider() { return (NoFrameProviderImpl&)WebSocket::provider(); }
        std::mutex _mutex;
        int _curOpCode;
        fleece::alloc_slice _curMessage;
        size_t _curMessageLength;
        size_t _bufferedBytes{ 0 };
        Stopwatch _timeConnected{ false };
        uint64_t _bytesSent{ 0 }, _bytesReceived{ 0 };
        bool _closeSent{ false }, _closeReceived{ false };
        fleece::alloc_slice _closeMessage;
    };


    class C4Provider : public NoFrameProviderImpl {
    public:
        C4Provider(C4SocketFactory f)
        :factory(f)
        { }

        virtual WebSocket* createWebSocket(const Address &address) override {
            return new C4SocketImpl(*this, address);
        }

        static void registerFactory(const C4SocketFactory &factory) {
            if (sInstance)
                throw new std::logic_error("c4socket_registerFactory can only be called once");
            sInstance = new C4Provider(factory);
        }

        static C4Provider &instance() {
            if (!sInstance)
                throw new std::logic_error("No C4SocketFactory has been registered yet!");
            return *sInstance;
        }

        virtual void openSocket(WebSocket *s) override {
            auto &address = s->address();
            C4Address c4addr = {
                slice(address.scheme),
                slice(address.hostname),
                address.port,
                slice(address.path)
            };
            factory.open((C4SocketImpl*)s, &c4addr);
        }

        virtual void closeSocket(WebSocket *s) override {
            factory.close((C4SocketImpl*)s);
        }

        virtual void sendBytes(WebSocket *s, alloc_slice bytes) override {
            bytes.retain();
            factory.write((C4SocketImpl*)s, {(void*)bytes.buf, bytes.size});
        }

        virtual void receiveComplete(WebSocket *s, size_t byteCount) override {
            factory.completedReceive((C4SocketImpl*)s, byteCount);
        }

    private:
        const C4SocketFactory factory;
        static C4Provider *sInstance;
    };

    C4Provider* C4Provider::sInstance;


    Provider& DefaultProvider() {
        return C4Provider::instance();
    }
    
} }


#pragma mark - PUBLIC C API:


static C4SocketImpl* internal(C4Socket *s)  {return (C4SocketImpl*)s;}


void c4socket_registerFactory(C4SocketFactory factory) C4API {
    C4Provider::registerFactory(factory);
}

void c4socket_opened(C4Socket *socket) C4API {
    internal(socket)->onConnect();
}

void c4socket_closed(C4Socket *socket, C4Error error) C4API {
    int err_no = 0;
    if (error.code)
        err_no = (error.domain == POSIXDomain) ? error.code : -1;   //FIX
    internal(socket)->onClose(err_no);
}

void c4socket_completedWrite(C4Socket *socket, size_t byteCount) C4API {
    internal(socket)->onWriteComplete(byteCount);
}

void c4socket_received(C4Socket *socket, C4Slice data) C4API {
    internal(socket)->onReceive(data);
}
