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
#include <memory>

namespace litecore { namespace blip {
    class BLIPIO;
    class ConnectionDelegate;
    class MessageOut;


    class Connection : public RefCounted {
    public:
        Connection(const std::string &hostname, uint16_t port,
                   WebSocketProvider &provider,
                   ConnectionDelegate&);
        virtual ~Connection();

        std::string name() const                                {return _name;}

        ConnectionDelegate& delegate() const                    {return _delegate;}

        MessageIn* sendRequest(MessageBuilder&);

        void close();

    protected:
        friend class MessageIn;
        friend class BLIPIO;

        void send(MessageOut*);

    private:
        std::string _name;
        ConnectionDelegate &_delegate;
        Retained<BLIPIO> _io;
    };


    class ConnectionDelegate {
    public:
        virtual ~ConnectionDelegate()  { }

        Connection* connection() const  {return _connection;}

        virtual void onConnect()                                {}
        virtual void onError(int errcode, fleece::slice reason) =0;
        virtual void onClose(int status, fleece::slice reason)  =0;
        virtual void onRequestReceived(MessageIn*)              =0;
        virtual void onResponseReceived(MessageIn*)             {}

    private:
        friend class Connection;
        
        Connection* _connection;
    };

} }
