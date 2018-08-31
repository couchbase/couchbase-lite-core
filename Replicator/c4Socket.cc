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

#include "fleece/Fleece.hh"
#include "c4.hh"
#include "c4Private.h"
#include "c4Socket.h"
#include "c4Socket+Internal.hh"
#include "Address.hh"
#include "Error.hh"
#include "WebSocketImpl.hh"
#include "StringUtil.hh"
#include <atomic>
#include <exception>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::repl;

CBL_CORE_API const char* const kC4SocketOptionWSProtocols = litecore::websocket::WebSocket::kProtocolsOption;


namespace litecore { namespace repl {

    using namespace websocket;


    static const C4SocketFactory& fac(const C4SocketFactory *f) {
        return f ? *f : C4SocketImpl::registeredFactory();
    }


    /** Implementation of C4Socket */
    C4SocketImpl::C4SocketImpl(websocket::URL url,
                               Role role,
                               alloc_slice options,
                               const C4SocketFactory *factory_,
                               void *nativeHandle_)
    :WebSocketImpl(url,
                   role,
                   AllocedDict(options),
                   fac(factory_).framing != kC4NoFraming)
    ,_factory(fac(factory_))
    {
        nativeHandle = nativeHandle_;
    }

    C4SocketImpl::~C4SocketImpl() {
        if (_factory.dispose)
            _factory.dispose(this);
    }


    WebSocket* WebSocketFrom(C4Socket *c4sock) {
        return c4sock ? (C4SocketImpl*)c4sock : nullptr;
    }


    void C4SocketImpl::validateFactory(const C4SocketFactory &f) {
#if DEBUG
        Assert(f.write != nullptr);
        Assert(f.completedReceive != nullptr);
        if (f.framing == kC4NoFraming) {
            Assert(f.close == nullptr);
            Assert(f.requestClose != nullptr);
        } else {
            Assert(f.close != nullptr);
            Assert(f.requestClose == nullptr);
        }
#endif
    }


    void C4SocketImpl::registerFactory(const C4SocketFactory &factory) {
        if (sRegisteredFactory)
            throw std::logic_error("c4socket_registerFactory can only be called once");
        validateFactory(factory);
        sRegisteredFactory = new C4SocketFactory(factory);
    }


    const C4SocketFactory& C4SocketImpl::registeredFactory() {
        if (!sRegisteredFactory)
            throw std::logic_error("No default C4SocketFactory registered; call c4socket_registerFactory())");
        return *sRegisteredFactory;
    }


    void C4SocketImpl::connect() {
        if (_factory.open) {
            Address c4addr(url());
            _factory.open(this, &c4addr, options().data(), _factory.context);
        }
    }

    void C4SocketImpl::requestClose(int status, fleece::slice message) {
        _factory.requestClose(this, status, message);
    }

    void C4SocketImpl::closeSocket() {
        _factory.close(this);
    }

    void C4SocketImpl::sendBytes(alloc_slice bytes) {
        _factory.write(this, C4SliceResult(bytes));
    }

    void C4SocketImpl::receiveComplete(size_t byteCount) {
        _factory.completedReceive(this, byteCount);
    }


    C4SocketFactory* C4SocketImpl::sRegisteredFactory;

} }


#pragma mark - PUBLIC C API:


static C4SocketImpl* internal(C4Socket *s)  {return (C4SocketImpl*)s;}


void c4socket_registerFactory(C4SocketFactory factory) C4API {
    C4SocketImpl::registerFactory(factory);
}

C4Socket* c4socket_fromNative(C4SocketFactory factory,
                              void *nativeHandle,
                              const C4Address *address) C4API
{
    return new C4SocketImpl(Address(*address).url(), Role::Server, {}, &factory, nativeHandle);
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
