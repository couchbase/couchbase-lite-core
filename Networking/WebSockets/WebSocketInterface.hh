//
// WebSocketInterface.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
#include "RefCounted.hh"
#include "InstanceCounted.hh"
#include "Logging.hh"
#include "fleece/Fleece.hh"
#include <atomic>
#include <map>
#include <string>

namespace litecore { namespace websocket {
    using fleece::RefCounted;
    using fleece::Retained;

    class WebSocket;
    class Delegate;
    class Headers;

    /** Reasons for a WebSocket closing. */
    enum CloseReason {
        kWebSocketClose,        // Closed by WebSocket protocol
        kPOSIXError,            // Closed due to IP socket error (see <errno.h>)
        kNetworkError,          // Closed due to other network error (see NetworkError below)
        kException,             // Closed due to an exception being thrown
        kUnknownError
    };

    /** Standardized WebSocket close codes. */
    enum CloseCode {
        kCodeNormal = 1000,
        kCodeGoingAway,
        kCodeProtocolError,
        kCodeUnsupportedData,
        kCodeStatusCodeExpected = 1005,     // Never sent
        kCodeAbnormal,                      // Never sent
        kCodeInconsistentData,
        kCodePolicyViolation,
        kCodeMessageTooBig,
        kCodeExtensionNotNegotiated,
        kCodeUnexpectedCondition,
        kCodeFailedTLSHandshake = 1015,
        kCloseAppTransient = 4001,          // App-defined transient error
        kCloseAppPermanent,                 // App-defined permanent error
    };

    enum NetworkError {
        kNetErrDNSFailure = 1,        // DNS lookup failed
        kNetErrUnknownHost,           // DNS server doesn't know the hostname
        kNetErrTimeout,
        kNetErrInvalidURL,
        kNetErrTooManyRedirects,
        kNetErrTLSHandshakeFailed,
        kNetErrTLSCertExpired,
        kNetErrTLSCertUntrusted,
        kNetErrTLSCertRequiredByPeer,
        kNetErrTLSCertRejectedByPeer,// 10
        kNetErrTLSCertUnknownRoot,
        kNetErrInvalidRedirect,
        kNetErrUnknown,              // Unknown error
        kNetErrTLSCertRevoked,
        kNetErrTLSCertNameMismatch,

        // Add new codes here. You MUST add messages to kLiteCoreMessages!
        // You MUST add corresponding kC4NetErr codes to the enum in C4Base.h!

        kNetErrorMaxPlus1
    };

    enum class Role {
        Client,
        Server
    };


    struct CloseStatus {
        CloseReason reason;
        int code;
        fleece::alloc_slice message;

        CloseStatus()
        :CloseStatus(kUnknownError, 0, fleece::nullslice) { }

        CloseStatus(CloseReason reason_, int code_, fleece::alloc_slice message_)
        :reason(reason_), code(code_), message(message_) { }

        CloseStatus(CloseReason reason_, int code_, fleece::slice message_)
        :CloseStatus(reason_, code_, fleece::alloc_slice(message_)) { }

        bool isNormal() const {
            return reason == kWebSocketClose && (code == kCodeNormal || code == kCodeGoingAway);
        }

        const char* reasonName() const;
    };


    /** "WS" log domain for WebSocket operations */
    extern LogDomain WSLogDomain;


    using URL = fleece::alloc_slice;


    /** Abstract class representing a WebSocket connection. */
    class WebSocket : public RefCounted, public fleece::InstanceCounted {
    public:
        const URL& url() const                      {return _url;}
        Role role() const                           {return _role;}
        Delegate& delegate() const;
        bool hasDelegate() const                    {return _delegate != nullptr;}

        virtual std::string name() const {
            return std::string(role() == Role::Server ? "<-" : "->") + (std::string)url();
        }

        /** Assigns the Delegate and opens the WebSocket. */
        void connect(Delegate *delegate);

        /** Sends a message. Callable from any thread.
            Returns false if the amount of buffered data is growing too large; the caller should
            then stop sending until it gets an onWebSocketWriteable delegate call. */
        virtual bool send(fleece::slice message, bool binary =true) =0;

        /** Closes the WebSocket. Callable from any thread. */
        virtual void close(int status =kCodeNormal, fleece::slice message =fleece::nullslice) =0;

    protected:
        WebSocket(const URL &url, Role role);
        virtual ~WebSocket();

        /** Called by the public connect(Delegate*) method. This should open the WebSocket. */
        virtual void connect() =0;

        /** Clears the delegate; any future calls to delegate() will fail. Call after closing. */
        void clearDelegate()                        {_delegate = nullptr;}
        
    private:
        const URL _url;
        const Role _role;
        Delegate *_delegate {nullptr};
    };


    class Message : public RefCounted {
    public:
        Message(fleece::slice d, bool b)        :data(d), binary(b) {}
        Message(fleece::alloc_slice d, bool b)  :data(d), binary(b) {}

        const fleece::alloc_slice data;
        const bool binary;
    };


    /** Mostly-abstract delegate interface for a WebSocket connection.
        Receives lifecycle events and incoming WebSocket messages.
        These callbacks are made on an undefined thread managed by the WebSocketProvider! */
    class Delegate {
    public:
        virtual ~Delegate() { }

        virtual void onWebSocketGotHTTPResponse(int status, const Headers &headers) { }
        virtual void onWebSocketConnect() =0;
        virtual void onWebSocketClose(CloseStatus) =0;

        /** A message has arrived. */
        virtual void onWebSocketMessage(Message*) =0;

        /** The socket has room to send more messages. */
        virtual void onWebSocketWriteable() { }
    };

} }
