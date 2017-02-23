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
        Connection(websocket::WebSocket*,
                   ConnectionDelegate&);

        virtual ~Connection();

        const std::string& name() const                         {return _name;}

        ConnectionDelegate& delegate() const                    {return _delegate;}

        /** Sends a built message as a new request.
            Returns a Future that will asynchronously provide a MessageIn object with the reply. */
        FutureResponse sendRequest(MessageBuilder&);

        typedef std::function<void(MessageIn*)> RequestHandler;

        /** Registers a callback that will be called when a message with a given profile arrives. */
        void setRequestHandler(std::string profile, RequestHandler);

        /** Closes the connection. */
        void close();

        bool isClosed()                                         {return _closed;}

#if DEBUG
        websocket::WebSocket* webSocket() const;
#endif

    protected:
        friend class MessageIn;
        friend class BLIPIO;

        void send(MessageOut*);
        void closed(bool normalClose, int code, fleece::slice reason);

    private:
        void start(websocket::WebSocket*);

        std::string _name;
        ConnectionDelegate &_delegate;
        Retained<BLIPIO> _io;
        std::atomic<bool> _closed {false};
    };


    /** Abstract interface of Connection delegates. The Connection calls these methods when
        lifecycle events happen, and when incoming messages arrive.
        The delegate methods are called on undefined threads, and should not block. */
    class ConnectionDelegate {
    public:
        virtual ~ConnectionDelegate()                           { }

        /** Called when the connection opens. */
        virtual void onConnect()                                { }

        /** Called when the connection closes, or fails to open.
            @param normalClose  True if the WebSocket closed cleanly; false on an error.
            @param status  A WebSocket status (on normal close) or POSIX errno (on error).
            @param reason  A message, if any, describing the status. */
        virtual void onClose(bool normalClose, int status, fleece::slice reason)  =0;

        /** Called when an incoming request is received. */
        virtual void onRequestReceived(MessageIn* request)      {request->notHandled();}

        /** Called when a response to an outgoing request arrives. */
        virtual void onResponseReceived(MessageIn*)             { }
    };

} }
