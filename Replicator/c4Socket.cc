//
// c4Socket.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "fleece/Fleece.hh"
#include "c4ExceptionUtils.hh"
#include "c4Private.h"
#include "c4Socket.hh"
#include "c4Socket+Internal.hh"
#include "Address.hh"
#include "Error.hh"
#include "Headers.hh"
#include "Logging.hh"
#include "WebSocketImpl.hh"
#include "StringUtil.hh"
#include <atomic>
#include <exception>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::net;
using namespace litecore::repl;
using namespace websocket;


static C4SocketFactory* sRegisteredFactory;
static C4SocketImpl::InternalFactory sRegisteredInternalFactory;


C4Socket::~C4Socket() = default;


void C4Socket::registerFactory(const C4SocketFactory &factory) {
    Assert(factory.write != nullptr && factory.completedReceive != nullptr);
    if (factory.framing == kC4NoFraming)
        Assert(factory.close == nullptr && factory.requestClose != nullptr);
    else
        Assert(factory.close != nullptr && factory.requestClose == nullptr);

    if (sRegisteredFactory)
        throw std::logic_error("c4socket_registerFactory can only be called once");
    sRegisteredFactory = new C4SocketFactory(factory);
}


C4Socket* C4Socket::fromNative(const C4SocketFactory &factory,
                               void *nativeHandle,
                               const C4Address &address)
{
    // Note: This should be wrapped in `retain()` since `C4SocketImpl` is ref-counted,
    // but doing so would cause client code to leak. Instead I added a warning to the doc-comment.
    return new C4SocketImpl(address.toURL(), Role::Server, {}, &factory, nativeHandle);
}


#pragma mark - C4SOCKETIMPL:


namespace litecore::repl {


    void C4SocketImpl::registerInternalFactory(C4SocketImpl::InternalFactory f) {
        sRegisteredInternalFactory = f;
    }


    Retained<WebSocket> CreateWebSocket(websocket::URL url,
                                        alloc_slice options,
                                        C4Database *database,
                                        const C4SocketFactory *factory,
                                        void *nativeHandle)
    {
        if (!factory)
            factory = sRegisteredFactory;
        
        if (factory) {
            auto ret = new C4SocketImpl(url, Role::Client, options, factory, nativeHandle);
            c4SocketTrace::traces().addEvent(ret, "CreateWebSocket", "C4SocketImpl");
            return ret;
        } else if (sRegisteredInternalFactory) {
            Assert(!nativeHandle);
            return sRegisteredInternalFactory(url, options, database);
        } else {
            throw std::logic_error("No default C4SocketFactory registered; call c4socket_registerFactory())");
        }
    }


    static const C4SocketFactory& effectiveFactory(const C4SocketFactory *f) {
        return f ? *f : C4SocketImpl::registeredFactory();
    }


    WebSocketImpl::Parameters C4SocketImpl::convertParams(slice c4SocketOptions) {
        WebSocketImpl::Parameters params = {};
        params.options = AllocedDict(c4SocketOptions);
        params.webSocketProtocols = params.options[kC4SocketOptionWSProtocols].asString();
        params.heartbeatSecs = (int)params.options[kC4ReplicatorHeartbeatInterval].asInt();
        return params;
    }


    C4SocketImpl::C4SocketImpl(websocket::URL url,
                               Role role,
                               alloc_slice options,
                               const C4SocketFactory *factory_,
                               void *nativeHandle_)
    :WebSocketImpl(url,
                   role,
                   effectiveFactory(factory_).framing != kC4NoFraming,
                   convertParams(options))
    ,_factory(effectiveFactory(factory_))
    {
        nativeHandle = nativeHandle_;
    }

    C4SocketImpl::~C4SocketImpl() {
        c4SocketTrace::traces().addEvent(this, "~C4SocketImpl");
        if (_factory.dispose)
            _factory.dispose(this);
    }


    WebSocket* WebSocketFrom(C4Socket *c4sock) {
        return c4sock ? (C4SocketImpl*)c4sock : nullptr;
    }


    const C4SocketFactory& C4SocketImpl::registeredFactory() {
        if (!sRegisteredFactory)
            throw std::logic_error("No default C4SocketFactory registered; call c4socket_registerFactory())");
        return *sRegisteredFactory;
    }


#pragma mark - WEBSOCKETIMPL OVERRIDES:


    void C4SocketImpl::connect() {
        WebSocketImpl::connect();
        c4SocketTrace::traces().addEvent(this, "connect", std::string("_factory.open is ") + (_factory.open == nullptr ? "null" : "set"));
        if (_factory.open) {
            net::Address c4addr(url());
            _factory.open(this, &c4addr, options().data(), _factory.context);
        }
    }

    void C4SocketImpl::requestClose(int status, fleece::slice message) {
        c4SocketTrace::traces().addEvent(this, "factory.requestClose");
        _factory.requestClose(this, status, message);
    }

    void C4SocketImpl::closeSocket() {
        c4SocketTrace::traces().addEvent(this, "factory.close");
        _factory.close(this);
    }

    void C4SocketImpl::closeWithException() {
        c4SocketTrace::traces().addEvent(this, "closeWithException");
        C4Error error = C4Error::fromCurrentException();
        WarnError("Closing socket due to C++ exception: %s\n%s",
                  error.description().c_str(), error.backtrace().c_str());
        close(kCodeUnexpectedCondition, "Internal exception"_sl);
    }

    void C4SocketImpl::sendBytes(alloc_slice bytes) {
        _factory.write(this, C4SliceResult(bytes));
    }

    void C4SocketImpl::receiveComplete(size_t byteCount) {
        _factory.completedReceive(this, byteCount);
    }


#pragma mark - C4SOCKET C++ API:


    void C4SocketImpl::gotHTTPResponse(int status, slice responseHeadersFleece) {
        try {
            Headers headers(responseHeadersFleece);
            WebSocketImpl::gotHTTPResponse(status, headers);
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::opened() {
        try {
            WebSocketImpl::onConnect();
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::closeRequested(int status, slice message) {
        try {
            WebSocketImpl::onCloseRequested(status, message);
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::closed(C4Error error) {
        try {
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

            WebSocketImpl::onClose(status);
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::completedWrite(size_t byteCount) {
        try {
            WebSocketImpl::onWriteComplete(byteCount);
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::received(slice data) {
        try {
            WebSocketImpl::onReceive(data);
        } catch (...) {closeWithException();}
    }

}


namespace c4SocketTrace {

    Event::Event(const C4Socket* sock, const string& f)
    : socket(sock)
    , tid(this_thread::get_id())
    , func(f)
    {
        timestamp = time_point_cast<microseconds>(system_clock::now()).time_since_epoch().count();
    }

    Event::Event(const C4Socket* sock, const string& f, const string& rem)
    : Event(sock, f)
    {
        remark = rem;
    }

    Event::operator string() {
        stringstream ss;
        ss << (void*)socket << ", time= " << timestamp << ", tid= " << tid << ", func= "
        << func;
        if (!remark.empty()) {
            ss << ", remark= " << remark;
        }
        return ss.str();
    }

    void EventQueue::addEvent(const C4Socket* sock, const string& f) {
        lock_guard<mutex> lock(mut);
        emplace_back(sock, f);
    }
    void EventQueue::addEvent(const C4Socket* sock, const string& f, const string& rem) {
        lock_guard<mutex> lock(mut);
        emplace_back(sock, f, rem);
    }

    EventQueue& traces() {
        static EventQueue* _traces = nullptr;
        if (_traces == nullptr) {
            _traces = new EventQueue();
        }
        return *_traces;
    }
}
