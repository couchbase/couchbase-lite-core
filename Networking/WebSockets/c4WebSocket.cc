//
// c4WebSocket.cc
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
#include "c4Socket.hh"
#include "c4WebSocket.hh"
#include "Address.hh"
#include "Error.hh"
#include "Headers.hh"
#include "Logging.hh"
#include "WebSocketImpl.hh"
#include <atomic>
#include <c4ExceptionUtils.hh>
#include <exception>
#include <utility>
#include <sstream>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::net;
using namespace litecore::repl;
using namespace websocket;

namespace litecore::repl {

    static C4WebSocket::InternalFactory sRegisteredInternalFactory;

    void C4WebSocket::registerInternalFactory(C4WebSocket::InternalFactory f) { sRegisteredInternalFactory = f; }

    Retained<WebSocket> CreateWebSocket(const websocket::URL& url, const alloc_slice& options,
                                        shared_ptr<DBAccess> database, const C4SocketFactory* factory,
                                        void* nativeHandle, C4KeyPair* externalKey) {
        if ( !factory && C4Socket::hasRegisteredFactory() ) factory = &C4Socket::registeredFactory();

        if ( factory ) {
            auto ret = new C4WebSocket(url, Role::Client, options, factory, nativeHandle);
            return ret;
        } else if ( sRegisteredInternalFactory ) {
            Assert(!nativeHandle);
            return sRegisteredInternalFactory(url, options, std::move(database), externalKey);
        } else {
            throw std::logic_error("No default C4SocketFactory registered; call c4socket_registerFactory())");
        }
    }

    static const C4SocketFactory& effectiveFactory(const C4SocketFactory* f) {
        return f ? *f : C4WebSocket::registeredFactory();
    }

    WebSocketImpl::Parameters C4WebSocket::convertParams(slice c4SocketOptions, C4KeyPair* externalKey) {
        WebSocketImpl::Parameters params = {};
        params.options                   = AllocedDict(c4SocketOptions);
        params.webSocketProtocols        = params.options[kC4SocketOptionWSProtocols].asString();
        params.heartbeatSecs             = (int)params.options[kC4ReplicatorHeartbeatInterval].asInt();
        params.networkInterface          = params.options[kC4SocketOptionNetworkInterface].asString();
#ifdef COUCHBASE_ENTERPRISE
        params.externalKey = externalKey;
#endif
        return params;
    }

    C4WebSocket::C4WebSocket(const URL& url, Role role, const alloc_slice& options, const C4SocketFactory* factory_,
                             void* nativeHandle_)
        : WebSocketImpl(url, role, effectiveFactory(factory_).framing != kC4NoFraming, convertParams(options))
        , C4Socket(effectiveFactory(factory_), nativeHandle_) {}

    WebSocket* WebSocketFrom(C4Socket* c4sock) { return dynamic_cast<C4WebSocket*>(c4sock); }

#pragma mark - WEBSOCKETIMPL OVERRIDES:

    void C4WebSocket::connect() {
        logInfo("**** connect");  //TEMP
        WebSocketImpl::connect();

        net::Address c4addr(url());
        _factory.open(this, (C4Address*)c4addr, options().data(), _factory.context);
    }

    alloc_slice C4WebSocket::peerTLSCertificateData() const {
        unique_lock lock(_mutex);
        return _peerCertData;
    }

    std::pair<int, Headers> C4WebSocket::httpResponse() const {
        unique_lock lock(_mutex);
        if ( _responseHeadersFleece ) return {_responseStatus, Headers(_responseHeadersFleece)};
        else
            return {_responseStatus, Headers()};
    }

    void C4WebSocket::requestClose(int status, fleece::slice message) { _factory.requestClose(this, status, message); }

    void C4WebSocket::closeSocket() { _factory.close(this); }

    void C4WebSocket::closeWithException() {
        C4Error error = C4Error::fromCurrentException();
        WarnError("Closing socket due to C++ exception: %s\n%s", error.description().c_str(),
                  error.backtrace().c_str());
        close(kCodeUnexpectedCondition, "Internal exception"_sl);
    }

    void C4WebSocket::sendBytes(alloc_slice bytes) { _factory.write(this, C4SliceResult(bytes)); }

    void C4WebSocket::receiveComplete(size_t byteCount) { _factory.completedReceive(this, byteCount); }

#pragma mark - C4SOCKET C++ API:

    bool C4WebSocket::hasCustomPeerCertValidation() const { return hasPeerCertValidator(); }

    bool C4WebSocket::gotPeerCertificate(slice certData, std::string_view hostname) {
        logInfo("**** gotPeerCertificate: %zu bytes", certData.size);  //TEMP
        try {
            {
                unique_lock lock(_mutex);
                _peerCertData = certData;
                _tlsHandshakeCondition.notify_all();  // wakes up waitForTLSHandshake()
            }
            // Call WebSocket's registered validator function, if any:
            if ( !validatePeerCert(certData, hostname) ) return false;

            // If `connect` has been called, notify the delegate now.
            // Otherwise there's no delegate yet, so wait until `opened` is called.
            notifyPeerCertificate();
            return true;
        }
        catchAndWarn() return false;
    }

    bool C4WebSocket::waitForTLSHandshake() {
        unique_lock lock(_mutex);
        _tlsHandshakeCondition.wait(lock, [this] { return _peerCertData != nullslice && !_closed; });
        return _peerCertData != nullslice;
    }

    void C4WebSocket::notifyPeerCertificate() {
        // Can't call `Delegate::onWebSocketGotTLSCertificate` until we have a delegate.
        if ( auto delegate = delegateWeak() ) {
            {
                unique_lock lock(_mutex);
                if ( _notifiedPeerCert ) return;
                _notifiedPeerCert = true;
            }
            if ( auto peerCertData = peerTLSCertificateData() ) {
                logInfo("**** notifying delegate of peer cert, %zu bytes", peerCertData.size);  //TEMP
                try {
                    delegate->invoke(&Delegate::onWebSocketGotTLSCertificate, peerCertData);
                }
                catchAndWarn()
            }
        }
    }

    void C4WebSocket::gotHTTPResponse(int status, slice responseHeadersFleece) {
        Assert(status >= 0);
        {
            unique_lock lock(_mutex);
            _responseStatus        = status;
            _responseHeadersFleece = responseHeadersFleece;
        }
    }

    void C4WebSocket::opened() {
        logInfo("**** opened");  //TEMP
        try {
            if ( hasPeerCertValidator() && !peerTLSCertificateData() ) {
                const char* message =
                        "WebSocket has peer cert validator but SocketFactory did not call gotPeerCertificate";
                WarnError("%s", message);
                close(kCodeUnexpectedCondition, message);
                return;
            }
            // Tell the delegate about the peer cert, if we didn't have a chance earlier:
            notifyPeerCertificate();

            WebSocketImpl::onConnect();
        } catch ( ... ) { closeWithException(); }
    }

    void C4WebSocket::closeRequested(int status, slice message) {
        try {
            WebSocketImpl::onCloseRequested(status, message);
        } catch ( ... ) { closeWithException(); }
    }

    void C4WebSocket::closed(C4Error error) {
        try {
            // Tell delegate about cert in the case where opened() was never called:
            notifyPeerCertificate();

            alloc_slice message = c4error_getMessage(error);
            CloseStatus status{kUnknownError, error.code, message};
            if ( error.code == 0 ) {
                status.reason = kWebSocketClose;
                status.code   = kCodeNormal;
            } else if ( error.domain == WebSocketDomain )
                status.reason = kWebSocketClose;
            else if ( error.domain == POSIXDomain )
                status.reason = kPOSIXError;
            else if ( error.domain == NetworkDomain )
                status.reason = kNetworkError;

            WebSocketImpl::onClose(status);
        } catch ( ... ) { closeWithException(); }

        {
            unique_lock lock(_mutex);
            _closed = true;
            _tlsHandshakeCondition.notify_all();
        }
    }

    void C4WebSocket::completedWrite(size_t byteCount) {
        try {
            WebSocketImpl::onWriteComplete(byteCount);
        } catch ( ... ) { closeWithException(); }
    }

    void C4WebSocket::received(slice data) {
        try {
            WebSocketImpl::onReceive(data);
        } catch ( ... ) { closeWithException(); }
    }

}  // namespace litecore::repl
