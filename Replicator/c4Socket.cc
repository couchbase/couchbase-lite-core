//
// c4Socket.cc
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


const char* const kC4SocketOptionWSProtocols = litecore::websocket::Provider::kProtocolsOption;


namespace litecore { namespace websocket {

    C4Address c4AddressFrom(const Address &address) {
        return C4Address {
            slice(address.scheme),
            slice(address.hostname),
            address.port,
            slice(address.path)
        };
    }


    websocket::Address addressFrom(const C4Address &addr) {
        return websocket::Address(asstring(addr.scheme),
                                  asstring(addr.hostname),
                                  addr.port);
    }

    
    websocket::Address addressFrom(const C4Address &addr, slice remoteDatabaseName) {
        stringstream path;
        path << addr.path;
        if (!slice(addr.path).hasSuffix("/"_sl))
            path << '/';
        path << remoteDatabaseName << "/_blipsync";
        return websocket::Address(asstring(addr.scheme),
                                  asstring(addr.hostname),
                                  addr.port,
                                  path.str());
    }


    websocket::Address addressFrom(C4Database* otherDB) {
        alloc_slice path(c4db_getPath(otherDB));
        return websocket::Address("file", "", 0, path.asString());
    }


#pragma mark - C4 SOCKET IMPL:


    /** Implementation of C4Socket */
    class C4SocketImpl : public WebSocketImpl, public C4Socket {
    public:
        C4SocketImpl(ProviderImpl &provider, const Address &address,
                     const AllocedDict &options,
                     const C4SocketFactory &factory_,
                     void *nativeHandle_ =nullptr)
        :WebSocketImpl(provider, address, options, !factory_.providesWebSockets)
        ,factory(factory_)
        {
            nativeHandle = nativeHandle_;
        }

        ~C4SocketImpl() {
            if (factory.dispose)
                factory.dispose(this);
        }


        void connect() override {    // called by base class's connect(Address)
            if (!nativeHandle)
                WebSocketImpl::connect();
        }

    private:
        friend class C4Provider;

        C4SocketFactory const factory;
    };


    WebSocket* WebSocketFrom(C4Socket *c4sock) {
        return c4sock ? (C4SocketImpl*)c4sock : nullptr;
    }


#pragma mark - C4 PROVIDER:


    void C4Provider::validateFactory(const C4SocketFactory &f) {
#if DEBUG
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

    WebSocketImpl* C4Provider::createWebSocket(const Address &address,
                                           const AllocedDict &options) {
        return new C4SocketImpl(*this, address, options, registeredFactory());
    }

    C4Socket* C4Provider::createWebSocket(const C4SocketFactory &factory,
                                  void *nativeHandle,
                                  const Address &address,
                                  const AllocedDict &options) {
        validateFactory(factory);
        return new C4SocketImpl(*this, address, options, factory, nativeHandle);
    }

    void C4Provider::registerFactory(const C4SocketFactory &factory) {
        if (sRegisteredFactory)
            throw std::logic_error("c4socket_registerFactory can only be called once");
        validateFactory(factory);
        sRegisteredFactory = new C4SocketFactory(factory);
    }

    C4SocketFactory& C4Provider::registeredFactory() {
        if (!sRegisteredFactory)
            registerFactory(C4DefaultSocketFactory);
        return *sRegisteredFactory;
    }

    C4Provider& C4Provider::instance() {
        static C4Provider* sInstance = new C4Provider();
        return *sInstance;
    }

    void C4Provider::openSocket(WebSocketImpl *s) {
        auto socket = (C4SocketImpl*)s;
        auto &address = socket->address();
        C4Address c4addr = {
            slice(address.scheme),
            slice(address.hostname),
            address.port,
            slice(address.path)
        };
        if (!socket->factory.open)
            error::_throw(error::UnsupportedOperation,
                          "C4SocketFactory does not support 'open'");
        socket->factory.open(socket, &c4addr, socket->options().data());
    }

    void C4Provider::requestClose(WebSocketImpl *s, int status, fleece::slice message) {
        auto socket = (C4SocketImpl*)s;
        socket->factory.requestClose((C4SocketImpl*)s, status, message);
    }

    void C4Provider::closeSocket(WebSocketImpl *s) {
        auto socket = (C4SocketImpl*)s;
        socket->factory.close((C4SocketImpl*)s);
    }

    void C4Provider::sendBytes(WebSocketImpl *s, alloc_slice bytes) {
        auto socket = (C4SocketImpl*)s;
        bytes.retain();
        socket->factory.write((C4SocketImpl*)s, {(void*)bytes.buf, bytes.size});
    }

    void C4Provider::receiveComplete(WebSocketImpl *s, size_t byteCount) {
        auto socket = (C4SocketImpl*)s;
        socket->factory.completedReceive((C4SocketImpl*)s, byteCount);
    }


    C4SocketFactory* C4Provider::sRegisteredFactory;

} }


#pragma mark - PUBLIC C API:


static C4SocketImpl* internal(C4Socket *s)  {return (C4SocketImpl*)s;}


void c4socket_registerFactory(C4SocketFactory factory) C4API {
    C4Provider::registerFactory(factory);
}

C4Socket* c4socket_fromNative(C4SocketFactory factory,
                              void *nativeHandle,
                              const C4Address *address) C4API
{
    return C4Provider::instance().createWebSocket(factory, nativeHandle, addressFrom(*address));
}

void c4socket_gotHTTPResponse(C4Socket *socket, int status, C4Slice responseHeadersFleece) C4API {
    AllocedDict headers((slice)responseHeadersFleece);
    internal(socket)->gotHTTPResponse(status, headers);
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
    if (error.code == 0) {
        status.reason = kWebSocketClose;
        status.code = kCodeNormal;
    } else if (error.domain == WebSocketDomain)
        status.reason = kWebSocketClose;
    else if (error.domain == POSIXDomain)
        status.reason = kPOSIXError;
    else if (error.domain == NetworkDomain)
        status.reason = kNetworkError;

    internal(socket)->onClose(status);
}

void c4socket_completedWrite(C4Socket *socket, size_t byteCount) C4API {
    internal(socket)->onWriteComplete(byteCount);
}

void c4socket_received(C4Socket *socket, C4Slice data) C4API {
    internal(socket)->onReceive(data);
}
