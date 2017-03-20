//
//  WebSocketImpl.cc
//  StreamTaskTest
//
//  Created by Jens Alfke on 3/15/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "WebSocketImpl.hh"
#include "WebSocketProtocol.hh"
#include <string>

using namespace fleece;

// The rest of the implementation of uWS::WebSocketProtocol, which calls into WebSocket:
namespace uWS {

    static constexpr size_t kMaxMessageLength = 1<<20;

    static constexpr size_t kSendBufferSize = 64 * 1024;


    // The `user` parameter points to the owning WebSocketImpl object.
    #define _sock ((litecore::websocket::WebSocketImpl*)user)


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::setCompressed(void *user) {
        return false;   //TODO: Implement compression
    }


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::refusePayloadLength(void *user, int length) {
        return length > kMaxMessageLength;
    }


    template <const bool isServer>
    void WebSocketProtocol<isServer>::forceClose(void *user) {
        _sock->disconnect();
    }


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::handleFragment(char *data,
                                                     size_t length,
                                                     unsigned int remainingBytes,
                                                     int opCode,
                                                     bool fin,
                                                     void *user)
    {
        // WebSocketProtocol expects this method to return true on error, but this confuses me
        // so I'm having my code return false on error, hence the `!`. --jpa
        return ! _sock->handleFragment(data, length, remainingBytes, opCode, fin);
    }


    // Explicitly generate code for template methods:
    
    //template class WebSocketProtocol<SERVER>;
    template class WebSocketProtocol<CLIENT>;
    
}


#pragma mark - WEBSOCKETIMPL:


// Implementation of WebSocketImpl:
namespace litecore { namespace websocket {

    using namespace uWS;

    WebSocketImpl::WebSocketImpl(ProviderImpl &provider, const Address &address)
    :WebSocket(provider, address)
    ,_protocol(new ClientProtocol)
    { }

    WebSocketImpl::~WebSocketImpl()
    { }


    void WebSocketImpl::connect() {    // called by base class's connect(Address)
        provider().openSocket(this);
    }

    void WebSocketImpl::disconnect() {
        provider().closeSocket(this);
    }

    void WebSocketImpl::close(int status, fleece::slice message) {
        char buf[2 + message.size];
        auto size = WebSocketProtocol<false>::formatClosePayload(buf, (uint16_t)status,
                                                                 (char*)message.buf, message.size);
        sendOp(slice(buf, size), uWS::CLOSE);
    }

    bool WebSocketImpl::send(fleece::slice message, bool binary) {
        return sendOp(message, binary ? uWS::BINARY : uWS::TEXT);
    }

    bool WebSocketImpl::sendOp(fleece::slice message, int opcode) {
        alloc_slice frame(message.size + 10);
        frame.size = ClientProtocol::formatMessage((char*)frame.buf,
                                                   (const char*)message.buf, message.size,
                                                   (uWS::OpCode)opcode, message.size, false);
        auto newValue = (_bufferedBytes += frame.size);
        provider().sendBytes(this, frame);
        return newValue <= kSendBufferSize;
    }


    void WebSocketImpl::onWriteComplete(size_t size) {
        auto newValue = (_bufferedBytes -= size);
        if (newValue <= kSendBufferSize && newValue + size > kSendBufferSize)
            delegate().onWebSocketWriteable();
    }


    void WebSocketImpl::onReceive(slice data) {
        std::lock_guard<std::mutex> lock(_mutex);
        _protocol->consume((char*)data.buf, (unsigned)data.size, this);
        // ... this will call handleFragment(), below
        provider().receiveComplete(this, data.size);
    }


    // Called from inside _protocol->consume()
    bool WebSocketImpl::handleFragment(char *data,
                                       size_t length,
                                       unsigned int remainingBytes,
                                       int opCode,
                                       bool fin)
    {
        // Beginning:
        if (!_curMessage) {
            _curOpCode = opCode;
            _curMessageCapacity = length + remainingBytes;
            _curMessage.reset(_curMessageCapacity);
            _curMessage.size = 0;
        }

        // Body:
        if (_curMessage.size + length > _curMessageCapacity)
            return false; // overflow!
        memcpy((void*)_curMessage.end(), data, length);
        _curMessage.size += length;

        // End:
        if (fin && remainingBytes == 0) {
            return receivedMessage(_curOpCode, std::move(_curMessage));
            assert(!_curMessage);
        }
        return true;
    }


    bool WebSocketImpl::receivedMessage(int opCode, alloc_slice message) {
        switch (opCode) {
            case TEXT:
                if (!ClientProtocol::isValidUtf8((unsigned char*)message.buf,
                                                            message.size))
                    return false;
                // fall through:
            case BINARY:
                delegate().onWebSocketMessage(message, (opCode==BINARY));
                return true;
            case CLOSE: {
                auto close = ClientProtocol::parseClosePayload((char*)message.buf,
                                                                          message.size);
                delegate().onWebSocketClose({kWebSocketClose,
                                             close.code,
                                             alloc_slice(close.message, close.length)});
                return false; // close the socket
            }
            case PING:
                send(message, PONG);
                return true;
            case PONG:
                //receivedPong(message);
                return true;
            default:
                return false;
        }
    }

} }
