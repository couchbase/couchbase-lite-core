//
// c4Socket+Internal.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4Socket.hh"
#include "WebSocketImpl.hh"

struct c4Database;

namespace litecore::repl {
    class DBAccess;

    // Main factory function to create a WebSocket.
    fleece::Retained<websocket::WebSocket> CreateWebSocket(const websocket::URL&, const fleece::alloc_slice& options,
                                                           std::shared_ptr<DBAccess>, const C4SocketFactory*,
                                                           void*      nativeHandle = nullptr,
                                                           C4KeyPair* externalKey  = nullptr);

    // Returns the WebSocket object associated with a C4Socket
    websocket::WebSocket* WebSocketFrom(C4Socket* c4sock);

    /** Implementation of C4Socket */
    class C4SocketImpl final
        : public websocket::WebSocketImpl
        , public C4Socket {
      public:
        static const C4SocketFactory& registeredFactory();

        using InternalFactory = websocket::WebSocketImpl* (*)(websocket::URL, fleece::alloc_slice   options,
                                                              std::shared_ptr<DBAccess>, C4KeyPair* externalKey);
        static void registerInternalFactory(InternalFactory);

        static Parameters convertParams(fleece::slice c4SocketOptions, C4KeyPair* externalKey = nullptr);

        C4SocketImpl(const websocket::URL&, websocket::Role, const fleece::alloc_slice& options, const C4SocketFactory*,
                     void* nativeHandle = nullptr);

        ~C4SocketImpl() override;

        void closeWithException();

        // WebSocket public API:
        void connect() override;

        std::pair<int, websocket::Headers> httpResponse() const override;

        // C4Socket API:
        bool gotPeerCertificate(slice certData, std::string_view hostname) override;
        void gotHTTPResponse(int httpStatus, slice responseHeadersFleece) override;
        void opened() override;
        void closed(C4Error errorIfAny) override;
        void closeRequested(int status, slice message) override;
        void completedWrite(size_t byteCount) override;
        void received(slice data) override;

      protected:
        std::string loggingClassName() const override { return "C4Socket"; }

        // WebSocket protected API:
        void requestClose(int status, fleece::slice message) override;
        void closeSocket() override;
        void sendBytes(fleece::alloc_slice bytes) override;
        void receiveComplete(size_t byteCount) override;

      private:
        C4SocketFactory const _factory;
        mutable std::mutex    _mutex;
        int                   _responseStatus = 0;
        alloc_slice           _responseHeadersFleece;
        alloc_slice           _peerCertData;
    };
}  // namespace litecore::repl
