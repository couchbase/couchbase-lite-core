//
// c4Socket.cc
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4Socket.hh"
#include "c4WebSocket.hh"
#include "Address.hh"
#include "TLSCodec.hh"
#include "TLSContext.hh"
#include "Error.hh"

using namespace litecore;
using websocket::Role;


static C4SocketFactory* sRegisteredFactory;

void C4Socket::registerFactory(const C4SocketFactory& factory) {
    Assert(factory.write != nullptr && factory.completedReceive != nullptr);
    if ( factory.framing == kC4NoFraming ) Assert(factory.close == nullptr && factory.requestClose != nullptr);
    else
        Assert(factory.close != nullptr && factory.requestClose == nullptr);

    if ( sRegisteredFactory ) throw std::logic_error("c4socket_registerFactory can only be called once");
    sRegisteredFactory = new C4SocketFactory(factory);
}

bool C4Socket::hasRegisteredFactory() { return sRegisteredFactory != nullptr; }

const C4SocketFactory& C4Socket::registeredFactory() {
    if ( !sRegisteredFactory )
        throw std::logic_error("No default C4SocketFactory registered; call c4socket_registerFactory())");
    return *sRegisteredFactory;
}

C4Socket* C4Socket::fromNative(const C4SocketFactory& factoryRef, void* nativeHandle, const C4Address& address,
                               bool incoming, C4TLSConfig* incomingTLSConfig) {
    C4SocketFactory const* factoryPtr = &factoryRef;
    C4SocketFactory        tlsFactory;
    if ( incomingTLSConfig ) {
        auto tlsContext = net::TLSContext::fromListenerOptions(incomingTLSConfig, (C4Listener*)nativeHandle);
        std::tie(tlsFactory, nativeHandle) = net::wrapSocketFactoryInTLS(factoryRef, nativeHandle, tlsContext);
        factoryPtr                         = &tlsFactory;
    }
    // Note: This should be wrapped in `retain()` since `C4WebSocket` is ref-counted,
    // but doing so would cause client code to leak. Instead I added a warning to the doc-comment.
    auto socket = new repl::C4WebSocket(address.toURL(), incoming ? Role::Server : Role::Client, {}, factoryPtr,
                                        nativeHandle);
    if ( factoryRef.attached ) factoryRef.attached(socket);
    return socket;
}

C4Socket::C4Socket(const C4SocketFactory& factory, void* nativeHandle)
    : _factory{factory}, _nativeHandle{nativeHandle} {}

C4Socket::~C4Socket() {
    if ( _factory.dispose ) _factory.dispose(this);
}

#pragma mark - C4SOCKETFACTORYIMPL

C4SocketFactory C4SocketFactoryImpl::factory() {
    C4SocketFactory fac = kFactory;
    fac.context         = this;
    return fac;
}

void C4SocketFactoryImpl::opened(C4Socket* socket) {
    if ( !_socket ) {
        _socket = socket;
    } else {
        Assert(socket == _socket);
    }
    if ( !socket->getNativeHandle() ) {
        socket->setNativeHandle(this);
        retain(this);  // balanced by the release in kFactory.dispose below
    }
}

void C4SocketFactoryImpl::attached() {
    DebugAssert(_socket);
    retain(this);  // balanced by the release in kFactory.dispose below
}

C4SocketFactoryImpl* C4SocketFactoryImpl::nativeHandle(C4Socket* socket) {
    return static_cast<C4SocketFactoryImpl*>(socket->getNativeHandle());
}

const C4SocketFactory C4SocketFactoryImpl::kFactory{
        .framing = kC4WebSocketClientFraming,
        .open =
                [](C4Socket* socket, const C4Address* addr, C4Slice options, void* context) {
                    auto impl     = static_cast<C4SocketFactoryImpl*>(context);
                    impl->_socket = socket;  // Ensure impl's socket ref is set
                    impl->open(socket, *addr, options);
                },
        .write            = [](C4Socket*     socket,
                    C4SliceResult allocatedData) { nativeHandle(socket)->write(alloc_slice(allocatedData)); },
        .completedReceive = [](C4Socket* socket,
                               size_t    byteCount) { nativeHandle(socket)->completedReceive(byteCount); },
        .close            = [](C4Socket* socket) { nativeHandle(socket)->close(); },
        .dispose          = [](C4Socket* socket) { release(nativeHandle(socket)); },
        .attached =
                [](C4Socket* socket) {
                    auto impl     = nativeHandle(socket);
                    impl->_socket = socket;  // Ensure impl's socket ref is set
                    impl->attached();
                }};
