//
// Server.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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
#include "RefCounted.hh"
#include "Request.hh"
#include "c4Base.h"
#include "c4Socket.h"
#include <array>
#include <map>
#include <mutex>
#include <functional>
#include <thread>
#include <vector>
#include <regex>

namespace sockpp {
    class acceptor;
    class mbedtls_context;
}
namespace litecore { namespace crypto {
    class Identity;
} }

namespace litecore { namespace REST {

    /** HTTP server, extending LWSServer to add configurable URI handlers. */
    class Server : public fleece::RefCounted {
    public:
        Server();

        void start(uint16_t port, const char *hostname =nullptr, crypto::Identity* =nullptr);

        virtual void stop();

        C4Address address() const;

        /** Extra HTTP headers to add to every response. */
        void setExtraHeaders(const std::map<std::string, std::string> &headers);

        /** A function that handles a request. */
        using Handler = std::function<void(RequestResponse&)>;

        /** Registers a handler function for a URI pattern.
            Patterns use glob syntax: <http://man7.org/linux/man-pages/man7/glob.7.html>
            Multiple patterns can be joined with a "|".
            Patterns are tested in the order the handlers are added, and the first match is used.*/
        void addHandler(net::Methods, const std::string &pattern, const Handler&);

    protected:
        struct URIRule {
            net::Methods     methods;
            std::string pattern;
            std::regex  regex;
            Handler     handler;
        };

        URIRule* findRule(net::Method method, const std::string &path);
        ~Server() = default;

        void dispatchRequest(RequestResponse*);

    private:
        void acceptConnections();

        fleece::Retained<crypto::Identity> _identity;
        std::unique_ptr<sockpp::mbedtls_context> _tlsContext;
        std::unique_ptr<sockpp::acceptor> _acceptor;
        std::thread _acceptThread;
        std::mutex _mutex;
        std::vector<URIRule> _rules;
        std::map<std::string, std::string> _extraHeaders;
        uint16_t _port;
    };

} }
