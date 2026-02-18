//
// WebSocketImpl.hh
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
#include "WebSocketInterface.hh"
#include "c4Certificate.hh"
#include "fleece/Expert.hh"  // for AllocedDict
#include "Logging.hh"
#include <memory>

namespace litecore::websocket {

    /** Transport-agnostic implementation of WebSocket protocol.
        It doesn't transfer data or run the handshake; it just knows how to encode and decode
        messages. */
    class WebSocketImpl
        : public WebSocket
        , public Logging {
      public:
        struct Parameters {
            alloc_slice webSocketProtocols;  ///< Sec-WebSocket-Protocol value
            int         heartbeatSecs;       ///< WebSocket heartbeat interval in seconds (default if 0)
            alloc_slice networkInterface;    ///< Network interface
            AllocedDict options;             ///< Other options
#ifdef COUCHBASE_ENTERPRISE
            Retained<C4KeyPair> externalKey;  ///< Client cert uses external key..
#endif
        };

        WebSocketImpl(const URL& url, Role role, bool framing, Parameters);

        void connect() override;
        bool send(slice message, bool binary = true) override;
        void close(int status = kCodeNormal, slice message = nullslice) override;

        const Parameters& parameters() const;

        const AllocedDict& options() const;

        struct impl;

      protected:
        // Timeout for WebSocket connection (until HTTP response received)
        static constexpr long kConnectTimeoutSecs = 15;

        ~WebSocketImpl() override;

        std::string loggingClassName() const override { return "WebSocket"; }

        std::string loggingIdentifier() const override;

        // Subclasses need to call these:
        void onConnect();
        void onCloseRequested(int status, slice message);
        void onClose(int posixErrno);
        void onClose(CloseStatus);
        void onReceive(slice);
        void onWriteComplete(size_t);

        // These methods have to be implemented in subclasses:
        virtual void closeSocket()                           = 0;
        virtual void sendBytes(alloc_slice)                  = 0;
        virtual void receiveComplete(size_t byteCount)       = 0;
        virtual void requestClose(int status, slice message) = 0;

      private:
        friend class MessageImpl;

        std::unique_ptr<impl> _impl;
    };

}  // namespace litecore::websocket
