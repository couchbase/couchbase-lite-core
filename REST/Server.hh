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
#include "fleece/RefCounted.hh"
#include "fleece/InstanceCounted.hh"
#include "Request.hh"
#include "StringUtil.hh"
#include <atomic>
#include <map>
#include <mutex>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <regex>

namespace sockpp {
    class acceptor;
    class inet_address;
    class stream_socket;
}  // namespace sockpp

namespace litecore::crypto {
    struct Identity;
}

namespace litecore::net {
    class TLSContext;
}

namespace litecore::REST {

    using namespace fleece;

    /** HTTP server with configurable URI handlers. */
    class Server final
        : public fleece::RefCounted
        , public fleece::InstanceCountedIn<Server> {
      public:
        Server();

        void start(uint16_t port, slice networkInterface = nullslice, net::TLSContext* = nullptr);

        virtual void stop();

        /** The port the Server is listening on. */
        uint16_t port() const;

        /** The IP address(es) of the Server. Generally these are numeric strings like "10.0.0.5",
            but they may also be hostnames if known. A hostname may be an mDNS/Bonjour hostname like
            "norbert.local". */
        std::vector<std::string> addresses() const;

        /** A function that authenticates an HTTP request, given the "Authorization" header. */
        using Authenticator = std::function<bool(slice)>;

        void setAuthenticator(Authenticator auth) { _authenticator = std::move(auth); }

        /** Extra HTTP headers to add to every response. */
        void setExtraHeaders(const std::map<std::string, std::string>& headers);

        /** A function that handles a request. */
        using Handler = std::function<void(RequestResponse&)>;

        /** Registers a handler function for a URI pattern.
            Patterns use glob syntax: <http://man7.org/linux/man-pages/man7/glob.7.html>
            Multiple patterns can be joined with a "|".
            Patterns are tested in the order the handlers are added, and the first match is used.*/
        void addHandler(net::Methods, const std::string& pattern, const Handler&);

        int connectionCount() { return _connectionCount; }

      protected:
        struct URIRule {
            net::Methods methods;
            std::string  pattern;
            std::regex   regex;
            Handler      handler;
        };

        URIRule* findRule(net::Method method, const std::string& path);
        ~Server() override;

        void dispatchRequest(RequestResponse*);

      private:
        void awaitConnection();
        void acceptConnection();
        void handleConnection(sockpp::stream_socket&&);

        fleece::Retained<crypto::Identity> _identity;
        fleece::Retained<net::TLSContext>  _tlsContext;
        std::unique_ptr<sockpp::acceptor>  _acceptor;
        std::mutex                         _mutex;
        std::vector<URIRule>               _rules;
        std::map<std::string, std::string> _extraHeaders;
        std::atomic<int>                   _connectionCount{0};
        Authenticator                      _authenticator;
    };

}  // namespace litecore::REST
