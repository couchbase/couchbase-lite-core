//
// BLIPConnection.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "WebSocketInterface.hh"
#include "Message.hh"
#include "Logging.hh"
#include "fleece/Expert.hh"  // for AllocedDict
#include <atomic>
#include <functional>

namespace litecore::blip {
    class BLIPIO;
    class ConnectionDelegate;
    class MessageOut;

    /** A BLIP connection. Use this object to open and close connections and send requests.
        The connection notifies about events and messages by calling its delegate.
        The methods are thread-safe. */
    class Connection final
        : public RefCounted
        , public Logging {
      public:
        enum State {
            kDisconnected = -1,
            kClosed       = 0,
            kConnecting,
            kConnected,
            kClosing,
        };

        using CloseStatus = websocket::CloseStatus;

        /** WebSocket 'protocol' name for BLIP; use as value of kProtocolsOption option. */
        static constexpr const char* kWSProtocolName = "BLIP_3";

        /** Option to set the 'deflate' compression level. Value must be an integer in the range
            0 (no compression) to 9 (best compression). */
        static constexpr const char* kCompressionLevelOption = "BLIPCompressionLevel";

        /** Creates a BLIP connection on a WebSocket. */
        Connection(websocket::WebSocket*, const fleece::AllocedDict& options, Retained<WeakHolder<ConnectionDelegate>>);

        const std::string& name() const { return _name; }

        websocket::Role role() const { return _role; }

        Retained<WeakHolder<ConnectionDelegate>> delegateWeak() { return _weakDelegate; }

        void start(Retained<WeakHolder<blip::ConnectionDelegate>>);

        /** Tears down a Connection's state including any reference cycles.
            The Connection must have either already stopped, or never started. */
        void terminate();

        /** Sends a built message as a new request. */
        void sendRequest(MessageBuilder&);

        typedef std::function<void(MessageIn*)> RequestHandler;

        /** Registers a callback that will be called when a message with a given profile arrives. */
        void setRequestHandler(std::string profile, bool atBeginning, RequestHandler);

        /** Closes the connection. */
        void close(websocket::CloseCode = websocket::kCodeNormal, fleece::slice message = fleece::nullslice);

        State state() { return _state; }

        std::string loggingIdentifier() const override { return _name; }

        /** Exposed only for testing. */
        websocket::WebSocket* webSocket() const;

      protected:
        ~Connection() override;

        friend class MessageIn;
        friend class BLIPIO;

        void send(MessageOut*);
        void gotHTTPResponse(int status, const websocket::Headers& headers);
        void connected();
        void closed(const CloseStatus&);

      private:
        std::string                              _name;
        websocket::Role const                    _role;
        Retained<WeakHolder<ConnectionDelegate>> _weakDelegate;
        Retained<BLIPIO>                         _io;
        int8_t                                   _compressionLevel;
        std::atomic<State>                       _state{kClosed};
        CloseStatus                              _closeStatus;
    };

    /** Abstract interface of Connection delegates. The Connection calls these methods when
        lifecycle events happen, and when incoming messages arrive.
        The delegate methods are called on undefined threads, and should not block. */
    class ConnectionDelegate {
      public:
        virtual ~ConnectionDelegate() = default;

        /** Called when the HTTP response arrives (just before onConnect or onClose). */
        virtual void onHTTPResponse(int status, const websocket::Headers& headers) {}

        /** Called when the connection opens. */
        virtual void onConnect() {}

        /** Called when the connection closes, or fails to open.
            @param status  The reason for the close, a status code, and a message.
            @param state  The Connection's new state: kDisconnected or kClosed. */
        virtual void onClose(Connection::CloseStatus status, Connection::State state) = 0;

        /** Called when the beginning of an incoming request arrives. The properties will be
            complete, but the body is likely to be incomplete. */
        virtual void onRequestBeginning(MessageIn* request) {}

        /** Called when an incoming request is completely received. */
        virtual void onRequestReceived(MessageIn* request) { request->notHandled(); }
    };

}  // namespace litecore::blip
