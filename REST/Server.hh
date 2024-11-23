//
// Server.hh
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
#include "c4Compat.h"
#include "fleece/RefCounted.hh"
#include "fleece/InstanceCounted.hh"
#include "fleece/slice.hh"
#include <atomic>
#include <map>
#include <mutex>
#include <memory>
#include <vector>

#ifdef COUCHBASE_ENTERPRISE

C4_ASSUME_NONNULL_BEGIN

namespace sockpp {
    class acceptor;
    class inet_address;
    class stream_socket;
}  // namespace sockpp

namespace litecore::crypto {
    struct Identity;
}

namespace litecore::net {
    class ResponderSocket;
    class TLSContext;
}  // namespace litecore::net

namespace litecore::REST {

    using namespace fleece;

    /** A basic TCP server. Incoming TCP connections are passed on to its delegate. */
    class Server final
        : public RefCounted
        , public InstanceCountedIn<Server> {
      public:
        class Delegate {
          public:
            virtual ~Delegate()                                                  = default;
            virtual void handleConnection(std::unique_ptr<net::ResponderSocket>) = 0;
        };

        explicit Server(Delegate&);

        void start(uint16_t port, slice networkInterface = {}, net::TLSContext* = nullptr);

        virtual void stop();

        /** The port the Server is listening on. */
        uint16_t port() const;

        /** The IP address(es) of the Server. Generally these are numeric strings like "10.0.0.5",
            but they may also be hostnames if known. A hostname may be an mDNS/Bonjour hostname like
            "norbert.local". */
        std::vector<std::string> addresses() const;

        int connectionCount() { return _connectionCount; }

      private:
        ~Server() override;
        void awaitConnection();
        void acceptConnection();
        void handleConnection(sockpp::stream_socket&&);

        std::mutex                        _mutex;
        Delegate&                         _delegate;
        Retained<crypto::Identity>        _identity;
        Retained<net::TLSContext>         _tlsContext;
        std::unique_ptr<sockpp::acceptor> _acceptor;
        std::atomic<int>                  _connectionCount{0};
    };

}  // namespace litecore::REST

C4_ASSUME_NONNULL_END

#endif
