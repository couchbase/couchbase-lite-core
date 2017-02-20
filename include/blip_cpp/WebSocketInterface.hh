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
#include <string>

namespace litecore { namespace websocket {

    class Connection;
    class Delegate;


    struct Address {
        const std::string hostname;
        const uint16_t port;
        const std::string path;

        Address(const std::string &hostname_, uint16_t port_, const std::string &path_ ="/")
        :hostname(hostname_) ,port(port_) ,path(path_)
        { }

        operator std::string() const {
            char portStr[10];
            sprintf(portStr, ":%u", port);
            return hostname + portStr + path;
        }
    };


    /** Abstract class that can open WebSocket client connections. */
    class Provider {
    public:
        virtual ~Provider() { }
        virtual void addProtocol(const std::string &protocol) =0;
        virtual Connection* createConnection(const Address&) =0;
        virtual void close() { }
    };


    /** Abstract class representing a WebSocket client connection. */
    class Connection {
    public:
        virtual ~Connection();

        Provider& provider() const                  {return _provider;}
        const Address& address() const              {return _address;}
        Delegate& delegate() const                  {assert(_delegate); return *_delegate;}

        std::string name;

        /** If the Connection was created with no Delegate, this assigns the Delegate and
            opens the WebSocket. */
        inline void connect(Delegate *delegate);

        /** Sends a message. Callable from any thread. */
        virtual void send(fleece::slice message, bool binary =true) =0;

        /** Closes the WebSocket. Callable from any thread. */
        virtual void close(int status =1000, fleece::slice message =fleece::nullslice) =0;

    protected:
        friend class Provider;

        Connection(Provider&, const Address&);

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

        Connection* webSocketConnection() const     {return _connection;}

        virtual void onWebSocketStart() { }
        virtual void onWebSocketConnect() =0;
        virtual void onWebSocketError(int status, fleece::slice reason) =0;
        virtual void onWebSocketClose(int status, fleece::slice reason) =0;

        /** A message has arrived. */
        virtual void onWebSocketMessage(fleece::slice message, bool binary) =0;

        /** The socket has room to send more messages. */
        virtual void onWebSocketWriteable() { }

    private:
        Connection* _connection {nullptr};
        friend class Connection;
    };



    inline Connection::Connection(Provider &p, const Address &a)
    :_address(a)
    ,_provider(p)
    { }

    inline Connection::~Connection() {
        if (_delegate)
            _delegate->_connection = nullptr;
    }

    inline void Connection::connect(Delegate *delegate) {
        assert(!_delegate);
        assert(delegate);
        _delegate = delegate;
        delegate->_connection = this;
        if (name.empty())
            name = (std::string)_address;
        connect();
    }


} }
