//
//  WebSocketInterface.hh
//  LiteCore
//
//  Created by Jens Alfke on 12/30/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include <assert.h>
#include <sstream>
#include <string>

namespace litecore { namespace websocket {

    class WebSocket;
    class Delegate;


    /** Basically a low-budget URL class. */
    struct Address {
        const std::string scheme;
        const std::string hostname;
        const uint16_t port;
        const std::string path;

        Address(const std::string &scheme_, const std::string &hostname_,
                uint16_t port_ =0, const std::string &path_ ="/")
        :scheme(scheme_)
        ,hostname(hostname_)
        ,port(port_ ? port_ : defaultPort())
        ,path(path_)
        { }

        Address(const std::string &hostname_,
                uint16_t port_ =0, const std::string &path_ ="/")
        :Address("ws", hostname_, port_, path_)
        { }

        uint16_t defaultPort() const {
            return (scheme == "wss" || scheme == "https") ? 443 : 80;
        }

        operator std::string() const {
            std::stringstream result;
            result << scheme << ':' << hostname;
            if (port != defaultPort())
                result << ':' << port;
            if (path.empty() || path[0] != '/')
                result << '/';
            result << path;
            return result.str();
        }
    };


    /** Abstract class that can create WebSockets. */
    class Provider {
    public:
        virtual ~Provider() { }
        virtual void addProtocol(const std::string &protocol) =0;
        virtual WebSocket* createWebSocket(const Address&) =0;
        virtual void close() { }
    };


    /** Abstract class representing a WebSocket connection. */
    class WebSocket {
    public:
        virtual ~WebSocket();

        Provider& provider() const                  {return _provider;}
        const Address& address() const              {return _address;}
        Delegate& delegate() const                  {assert(_delegate); return *_delegate;}

        std::string name;

        /** If the WebSocket was created with no Delegate, this assigns the Delegate and
            opens the WebSocket. */
        inline void connect(Delegate *delegate);

        /** Sends a message. Callable from any thread. */
        virtual void send(fleece::slice message, bool binary =true) =0;

        /** Closes the WebSocket. Callable from any thread. */
        virtual void close(int status =1000, fleece::slice message =fleece::nullslice) =0;

    protected:
        friend class Provider;

        WebSocket(Provider&, const Address&);

        virtual void connect() =0;
        
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

        WebSocket* webSocket() const                {return _webSocket;}

        virtual void onWebSocketStart() { }
        virtual void onWebSocketConnect() =0;
        virtual void onWebSocketError(int status, fleece::slice reason) =0;
        virtual void onWebSocketClose(int status, fleece::slice reason) =0;

        /** A message has arrived. */
        virtual void onWebSocketMessage(fleece::slice message, bool binary) =0;

        /** The socket has room to send more messages. */
        virtual void onWebSocketWriteable() { }

    private:
        WebSocket* _webSocket {nullptr};
        friend class WebSocket;
    };



    inline WebSocket::WebSocket(Provider &p, const Address &a)
    :_address(a)
    ,_provider(p)
    { }

    inline WebSocket::~WebSocket() {
        if (_delegate)
            _delegate->_webSocket = nullptr;
    }

    inline void WebSocket::connect(Delegate *delegate) {
        assert(!_delegate);
        assert(delegate);
        _delegate = delegate;
        delegate->_webSocket = this;
        if (name.empty())
            name = (std::string)_address;
        connect();
    }


} }
