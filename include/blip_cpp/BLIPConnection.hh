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
#include "Logging.hh"
#include <atomic>

namespace litecore { namespace blip {
    class BLIPIO;
    class ConnectionDelegate;
    class MessageOut;


    /** A BLIP connection. Use this object to open and close connections and send requests.
        The connection notifies about events and messages by calling its delegate.
        The methods are thread-safe. */
    class Connection : public RefCounted, Logging {
    public:

        enum State {
            kDisconnected = -1,
            kClosed = 0,
            kConnecting,
            kConnected,
            kClosing,
        };

        using CloseStatus = websocket::CloseStatus;

        /** Creates a BLIP connection to an address, opening a WebSocket. */
        Connection(const websocket::Address&,
                   websocket::Provider &provider,
                   ConnectionDelegate&);

        /** Creates a BLIP connection on existing incoming WebSocket. */
        Connection(websocket::WebSocket*,
                   ConnectionDelegate&);

        virtual ~Connection();

        const std::string& name() const                         {return _name;}

        bool isServer() const                                   {return _isServer;}

        ConnectionDelegate& delegate() const                    {return _delegate;}

        /** Sends a built message as a new request. */
        void sendRequest(MessageBuilder&);

        typedef std::function<void(MessageIn*)> RequestHandler;

        /** Registers a callback that will be called when a message with a given profile arrives. */
        void setRequestHandler(std::string profile, bool atBeginning, RequestHandler);

        /** Closes the connection. */
        void close();

        State state()                                           {return _state;}

        virtual std::string loggingIdentifier() const override {return _name;}

        /** Exposed only for testing. */
        websocket::WebSocket* webSocket() const;

    protected:
        friend class MessageIn;
        friend class BLIPIO;

        void send(MessageOut*);
        void connected();
        void closed(CloseStatus);

    private:
        void start(websocket::WebSocket*);

        std::string _name;
        bool const _isServer;
        ConnectionDelegate &_delegate;
        Retained<BLIPIO> _io;
        std::atomic<State> _state {kClosed};
        CloseStatus _closeStatus;
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
            @param status  The reason for the close, a status code, and a message. */
        virtual void onClose(Connection::CloseStatus status)  =0;

        /** Called when the beginning of an incoming request arrives. The properties will be
            complete, but the body is likely to be incomplete. */
        virtual void onRequestBeginning(MessageIn* request)      { }

        /** Called when an incoming request is completely received. */
        virtual void onRequestReceived(MessageIn* request)      {request->notHandled();}
    };

} }
