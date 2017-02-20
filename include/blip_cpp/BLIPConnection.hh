//
//  BLIPConnection.hh
//  LiteCore
//
//  Created by Jens Alfke on 12/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketInterface.hh"
#include "Message.hh"

namespace litecore { namespace blip {
    class BLIPIO;
    class ConnectionDelegate;
    class MessageOut;


    /** A BLIP connection. Use this object to open and close connections and send requests.
        The connection notifies about events and messages by calling its delegate.
        The methods are thread-safe. */
    class Connection : public RefCounted {
    public:
        /** Creates a BLIP connection to an address, opening a WebSocket. */
        Connection(const websocket::Address&,
                   websocket::Provider &provider,
                   ConnectionDelegate&);

        /** Creates a BLIP connection on existing incoming WebSocket. */
        Connection(websocket::Connection *webSocket,
                   ConnectionDelegate&);

        virtual ~Connection();

        std::string name() const                                {return _name;}

        ConnectionDelegate& delegate() const                    {return _delegate;}

        /** Sends a built message as a new request.
            Returns a Future that will asynchronously provide a MessageIn object with the reply. */
        FutureResponse sendRequest(MessageBuilder&);

        typedef std::function<void(MessageIn*)> RequestHandler;

        /** Registers a callback that will be called when a message with a given profile arrives. */
        void setRequestHandler(std::string profile, RequestHandler);

        /** Closes the connection. */
        void close();

    protected:
        friend class MessageIn;
        friend class BLIPIO;

        void send(MessageOut*);

    private:
        void start(websocket::Connection*);

        std::string _name;
        ConnectionDelegate &_delegate;
        Retained<BLIPIO> _io;
    };


    /** Abstract interface of Connection delegates. The Connection calls these methods when
        lifecycle events happen, and when incoming messages arrive.
        The delegate methods are called on undefined threads, and should not block. */
    class ConnectionDelegate {
    public:
        virtual ~ConnectionDelegate()  { }

        Connection* connection() const  {return _connection;}

        /** Called when the connection opens. */
        virtual void onConnect()                                {}

        /** Called if a fatal error occurs, closing the connection, or if it failed to open. */
        virtual void onError(int errcode, fleece::slice reason) =0;

        /** Called when the connection closes. The WebSocket status/reason are given. */
        virtual void onClose(int status, fleece::slice reason)  =0;

        /** Called when an incoming request is received. */
        virtual void onRequestReceived(MessageIn* request)      {request->notHandled();}

        /** Called when a response to an outgoing request arrives. */
        virtual void onResponseReceived(MessageIn*)             {}

    private:
        friend class Connection;
        
        Connection* _connection;
    };

} }
