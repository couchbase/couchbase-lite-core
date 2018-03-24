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
#include "Address.hh"
#include "FleeceCpp.hh"
#include "Logging.hh"
#include "RefCounted.hh"
#include <assert.h>
#include <atomic>
#include <map>
#include <string>

namespace litecore { namespace websocket {

    class WebSocket;
    class Delegate;

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
        kNetErrTLSClientCertRequired,
        kNetErrTLSClientCertRejected, // 10
        kNetErrTLSCertUnknownRoot,
        kNetErrInvalidRedirect,
    };

    struct CloseStatus {
        CloseReason reason;
        int code;
        fleece::alloc_slice message;

        bool isNormal() const {
            return reason == kWebSocketClose && (code == kCodeNormal || code == kCodeGoingAway);
        }

        const char* reasonName() const  {
            static const char* kReasonNames[] = {"WebSocket status", "errno",
                                                 "Network error", "Exception", "Unknown error"};
            assert(reason < CloseReason(5));
            return kReasonNames[reason];
        }
    };


    /** "WS" log domain for WebSocket operations */
    extern LogDomain WSLogDomain;


    /** Abstract class that can create WebSockets. */
    class Provider {
    public:
        virtual ~Provider() { }
        virtual WebSocket* createWebSocket(const Address&,
                                           const fleeceapi::AllocedDict &options ={}) =0;
        virtual void close() { }

        static constexpr const char *kProtocolsOption = "WS-Protocols";     // string
        static constexpr const char *kHeartbeatOption = "heartbeat";        // seconds
    };


    /** Abstract class representing a WebSocket connection. */
    class WebSocket : public RefCounted {
    public:
        Provider& provider() const                  {return _provider;}
        const Address& address() const              {return _address;}
        Delegate& delegate() const                  {assert(_delegate); return *_delegate;}
        bool hasDelegate() const                    {return _delegate != nullptr;}

        std::string name;

        /** Assigns the Delegate and opens the WebSocket. */
        inline void connect(Delegate *delegate);

        /** Sends a message. Callable from any thread.
            Returns false if the amount of buffered data is growing too large; the caller should
            then stop sending until it gets an onWebSocketWriteable delegate call. */
        virtual bool send(fleece::slice message, bool binary =true) =0;

        /** Closes the WebSocket. Callable from any thread. */
        virtual void close(int status =kCodeNormal, fleece::slice message =fleece::nullslice) =0;

        /** The number of WebSocket instances in memory; for leak checking */
        static std::atomic_int gInstanceCount;

    protected:
        friend class Provider;

        WebSocket(Provider&, const Address&);
        virtual ~WebSocket();

        /** Called by the public connect(Delegate*) method. This should open the WebSocket. */
        virtual void connect() =0;

        /** Clears the delegate; any future calls to delegate() will fail. Call after closing. */
        void clearDelegate()                        {_delegate = nullptr;}
        
    private:
        const Address _address;
        Provider &_provider;
        Delegate *_delegate {nullptr};
    };


    /** Mostly-abstract delegate interface for a WebSocket connection.
        Receives lifecycle events and incoming WebSocket messages.
        These callbacks are made on an undefined thread managed by the WebSocketProvider! */
    class Delegate {
    public:
        virtual ~Delegate() { }

        virtual void onWebSocketStart() { }
        virtual void onWebSocketGotHTTPResponse(int status,
                                                const fleeceapi::AllocedDict &headers) { }
        virtual void onWebSocketConnect() =0;
        virtual void onWebSocketClose(CloseStatus) =0;

        /** A message has arrived. */
        virtual void onWebSocketMessage(fleece::slice message, bool binary) =0;

        /** The socket has room to send more messages. */
        virtual void onWebSocketWriteable() { }
    };



    inline WebSocket::WebSocket(Provider &p, const Address &a)
    :_address(a)
    ,_provider(p)
    {
        ++gInstanceCount;
    }

    inline WebSocket::~WebSocket() {
        --gInstanceCount;
    }

    inline void WebSocket::connect(Delegate *delegate) {
        assert(!_delegate);
        assert(delegate);
        _delegate = delegate;
        if (name.empty())
            name = (std::string)_address;
        connect();
    }


} }
