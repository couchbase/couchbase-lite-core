//
//  WebSocketInterface.hh
//  LiteCore
//
//  Created by Jens Alfke on 12/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Address.hh"
#include "FleeceCpp.hh"
#include <assert.h>
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
        kUnknownError
    };

    /** Standardized WebSocket close codes. */
    enum CloseCode {
        kCodeNormal = 1000,
        kCodeGoingAway,
        kCodeProtocolError,
        kCodeUnsupportedData,
        kCodeStatusCodeExpected = 1005,
        kCodeAbnormal,
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
                                                 "Network error", "Unknown error"};
            assert(reason < CloseReason(4));
            return kReasonNames[reason];
        }
    };


    /** Abstract class that can create WebSockets. */
    class Provider {
    public:
        virtual ~Provider() { }
        virtual void addProtocol(const std::string &protocol) =0;
        virtual WebSocket* createWebSocket(const Address&,
                                           const fleeceapi::AllocedDict &options ={}) =0;
        virtual void close() { }
    };


    /** Abstract class representing a WebSocket connection. */
    class WebSocket {
    public:
        virtual ~WebSocket() { }

        Provider& provider() const                  {return _provider;}
        const Address& address() const              {return _address;}
        Delegate& delegate() const                  {assert(_delegate); return *_delegate;}

        std::string name;

        /** Assigns the Delegate and opens the WebSocket. */
        inline void connect(Delegate *delegate);

        /** Sends a message. Callable from any thread.
            Returns false if the amount of buffered data is growing too large; the caller should
            then stop sending until it gets an onWebSocketWriteable delegate call. */
        virtual bool send(fleece::slice message, bool binary =true) =0;

        /** Closes the WebSocket. Callable from any thread. */
        virtual void close(int status =kCodeNormal, fleece::slice message =fleece::nullslice) =0;

    protected:
        friend class Provider;

        WebSocket(Provider&, const Address&);

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
    { }

    inline void WebSocket::connect(Delegate *delegate) {
        assert(!_delegate);
        assert(delegate);
        _delegate = delegate;
        if (name.empty())
            name = (std::string)_address;
        connect();
    }


} }
