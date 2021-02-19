//
// BLIPConnection.hh
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
#include "WebSocketInterface.hh"
#include "Message.hh"
#include "Logging.hh"
#include <atomic>
#include <functional>

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

        /** WebSocket 'protocol' name for BLIP; use as value of kProtocolsOption option. */
        static constexpr const char *kWSProtocolName = "BLIP_3";

        /** Option to set the 'deflate' compression level. Value must be an integer in the range
            0 (no compression) to 9 (best compression). */
        static constexpr const char *kCompressionLevelOption = "BLIPCompressionLevel";

        /** Creates a BLIP connection on a WebSocket. */
        Connection(websocket::WebSocket*,
                   const fleece::AllocedDict &options,
                   ConnectionDelegate&);

        const std::string& name() const                         {return _name;}

        websocket::Role role() const                            {return _role;}

        ConnectionDelegate& delegate() const                    {return _delegate;}

        void start();

        /** Tears down a Connection's state including any reference cycles.
            The Connection must have either already stopped, or never started. */
        void terminate();

        /** Sends a built message as a new request. */
        void sendRequest(MessageBuilder&);

        typedef std::function<void(MessageIn*)> RequestHandler;

        /** Registers a callback that will be called when a message with a given profile arrives. */
        void setRequestHandler(std::string profile, bool atBeginning, RequestHandler);

        /** Closes the connection. */
        void close(websocket::CloseCode =websocket::kCodeNormal,
                   fleece::slice message =fleece::nullslice);

        State state()                                           {return _state;}

        virtual std::string loggingIdentifier() const override  {return _name;}
        
        /** Exposed only for testing. */
        websocket::WebSocket* webSocket() const;

    protected:
        virtual ~Connection();

        friend class MessageIn;
        friend class BLIPIO;

        void send(MessageOut*);
        void gotHTTPResponse(int status, const websocket::Headers &headers);
        void gotTLSCertificate(slice certData);
        void connected();
        void closed(CloseStatus);

    private:
        std::string _name;
        websocket::Role const _role;
        ConnectionDelegate &_delegate;
        Retained<BLIPIO> _io;
        int8_t _compressionLevel;
        std::atomic<State> _state {kClosed};
        CloseStatus _closeStatus;
    };


    /** Abstract interface of Connection delegates. The Connection calls these methods when
        lifecycle events happen, and when incoming messages arrive.
        The delegate methods are called on undefined threads, and should not block. */
    class ConnectionDelegate {
    public:
        virtual ~ConnectionDelegate()                           { }

        /** Called when the HTTP response arrives (just before onConnect or onClose). */
        virtual void onHTTPResponse(int status, const websocket::Headers &headers) { }

        virtual void onTLSCertificate(slice certData) =0;

        /** Called when the connection opens. */
        virtual void onConnect()                                { }

        /** Called when the connection closes, or fails to open.
            @param status  The reason for the close, a status code, and a message.
            @param state  The Connection's new state: kDisconnected or kClosed. */
        virtual void onClose(Connection::CloseStatus status, Connection::State state)  =0;

        /** Called when the beginning of an incoming request arrives. The properties will be
            complete, but the body is likely to be incomplete. */
        virtual void onRequestBeginning(MessageIn* request)      { }

        /** Called when an incoming request is completely received. */
        virtual void onRequestReceived(MessageIn* request)      {request->notHandled();}
    };

} }
