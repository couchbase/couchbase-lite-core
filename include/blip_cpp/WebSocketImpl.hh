//
//  WebSocketImpl.hh
//  StreamTaskTest
//
//  Created by Jens Alfke on 3/15/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "WebSocketInterface.hh"
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <set>

namespace uWS {
    template <const bool isServer> class WebSocketProtocol;
}

namespace litecore { namespace websocket {
    class ProviderImpl;


    /** Transport-agnostic implementation of WebSocket protocol.
        It doesn't transfer data or run the handshake; it just knows how to encode and decode
        messages. */
    class WebSocketImpl : public WebSocket {
    public:

        WebSocketImpl(ProviderImpl&, const Address&);
        virtual ~WebSocketImpl();

        virtual bool send(fleece::slice message, bool binary =true) override;
        virtual void close(int status =1000, fleece::slice message =fleece::nullslice) override;

        // Concrete socket implementation needs to call these:
        void onConnect()                            {delegate().onWebSocketConnect();}
        void onClose(CloseStatus s)                 {delegate().onWebSocketClose(s);}
        void onReceive(fleece::slice s);
        void onWriteComplete(size_t);
        
    protected:
        ProviderImpl& provider()                    {return (ProviderImpl&)WebSocket::provider();}
        virtual void connect() override;
        void disconnect();

    private:
        template <const bool isServer>
        friend class uWS::WebSocketProtocol;

        using ClientProtocol = uWS::WebSocketProtocol<false>;

        bool sendOp(fleece::slice, int opcode);
        void _onReceive(fleece::alloc_slice);
        bool handleFragment(char *data,
                            size_t length,
                            unsigned int remainingBytes,
                            int opCode,
                            bool fin);
        bool receivedMessage(int opCode, fleece::alloc_slice message);

        std::unique_ptr<ClientProtocol> _protocol;
        std::mutex _mutex;
        int _curOpCode;
        fleece::alloc_slice _curMessage;
        size_t _curMessageCapacity;
        std::atomic<size_t> _bufferedBytes {0};
    };


    /** Provider implementation that creates WebSocketImpls. */
    class ProviderImpl : public Provider {
    public:
        ProviderImpl() { }

        virtual void addProtocol(const std::string &protocol) override {
            _protocols.insert(protocol);
        }

    protected:
        friend class WebSocketImpl;

        // These methods have to be implemented in subclasses, to connect to the actual socket:

        virtual void openSocket(WebSocketImpl*) =0;
        virtual void closeSocket(WebSocketImpl*) =0;
        virtual void sendBytes(WebSocketImpl*, fleece::alloc_slice) =0;
        virtual void receiveComplete(WebSocketImpl*, size_t byteCount) =0;

        std::set<std::string> _protocols;
    };

} }
