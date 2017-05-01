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
#include "Error.hh"
#include "WebSocketImpl.hh"
#include "StringUtil.hh"
#include <atomic>
#include <exception>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;
using namespace litecore::websocket;


namespace litecore { namespace websocket {

    class C4SocketImpl : public WebSocketImpl, public C4Socket {
    public:
        C4SocketImpl(ProviderImpl &provider, const Address &address, bool framing)
        :WebSocketImpl(provider, address, framing)
        {
            nativeHandle = nullptr;
        }
    };


    class C4Provider : public ProviderImpl {
    public:
        C4Provider(C4SocketFactory f)
        :_factory(f)
        {
#if DEBUG
            Assert(f.open != nullptr);
            Assert(f.write != nullptr);
            Assert(f.completedReceive != nullptr);
            if (f.providesWebSockets) {
                Assert(f.close == nullptr);
                Assert(f.requestClose != nullptr);
            } else {
                Assert(f.close != nullptr);
                Assert(f.requestClose == nullptr);
            }
#endif
        }

        virtual WebSocketImpl* createWebSocket(const Address &address) override {
            return new C4SocketImpl(*this, address, !_factory.providesWebSockets);
        }

        static void registerFactory(const C4SocketFactory &factory) {
            if (sInstance)
                throw std::logic_error("c4socket_registerFactory can only be called once");
            sInstance = new C4Provider(factory);
        }

        static C4Provider &instance() {
            if (!sInstance)
                registerFactory(C4DefaultSocketFactory);
            return *sInstance;
        }

        virtual void openSocket(WebSocketImpl *s) override {
            auto &address = s->address();
            C4Address c4addr = {
                slice(address.scheme),
                slice(address.hostname),
                address.port,
                slice(address.path)
            };
            _factory.open((C4SocketImpl*)s, &c4addr);
        }

        virtual void requestClose(WebSocketImpl *s, int status, fleece::slice message) override {
            _factory.requestClose((C4SocketImpl*)s, status, message);
        }

        virtual void closeSocket(WebSocketImpl *s) override {
            _factory.close((C4SocketImpl*)s);
        }

        virtual void sendBytes(WebSocketImpl *s, alloc_slice bytes) override {
            bytes.retain();
            _factory.write((C4SocketImpl*)s, {(void*)bytes.buf, bytes.size});
        }

        virtual void receiveComplete(WebSocketImpl *s, size_t byteCount) override {
            _factory.completedReceive((C4SocketImpl*)s, byteCount);
        }

    private:
        const C4SocketFactory _factory;

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

void c4socket_closeRequested(C4Socket *socket, int status, C4String message) {
    internal(socket)->onCloseRequested(status, message);
}

void c4socket_closed(C4Socket *socket, C4Error error) C4API {
    alloc_slice message = c4error_getMessage(error);
    CloseStatus status {kUnknownError, error.code, message};
    if (error.domain == WebSocketDomain)
        status.reason = kWebSocketClose;
    else if (error.domain == POSIXDomain)
        status.reason = kPOSIXError;
    else if (error.domain == DNSDomain)
        status.reason = kDNSError;
    internal(socket)->onClose(status);
}

void c4socket_completedWrite(C4Socket *socket, size_t byteCount) C4API {
    internal(socket)->onWriteComplete(byteCount);
}

void c4socket_received(C4Socket *socket, C4Slice data) C4API {
    internal(socket)->onReceive(data);
}
