//
// Response.hh
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
#include "Headers.hh"
#include "HTTPTypes.hh"
#include "MIMEType.hh"
#include "fleece/RefCounted.hh"
#include "fleece/slice.hh"
#include "fleece/Fleece.hh"
#include "c4Base.h"
#include <memory>
#include <utility>

struct C4Cert;
struct C4KeyPair;

namespace litecore::net {
    class HTTPLogic;
    struct ProxySpec;
    class TLSContext;
}  // namespace litecore::net

namespace litecore::REST {

    /** An incoming HTTP body. */
    class Body {
      public:
        using HTTPStatus = net::HTTPStatus;

        fleece::slice header(const char* name) const { return _headers[fleece::slice(name)]; }

        fleece::slice operator[](const char* name) const { return header(name); }

        std::optional<MIMEType> contentType() const;

        bool                hasContentType(fleece::slice hasType) const;
        fleece::alloc_slice body() const;
        fleece::Value       bodyAsJSON() const;

      protected:
        Body() = default;

        Body(websocket::Headers headers, fleece::alloc_slice body)
            : _headers(std::move(headers)), _body(std::move(body)) {}

        void setHeaders(const websocket::Headers& h) { _headers = h; }

        void setBody(fleece::alloc_slice body) { _body = std::move(body); }

        websocket::Headers              _headers;
        mutable std::optional<MIMEType> _contentType;
        fleece::alloc_slice             _body;
        mutable bool                    _gotBodyFleece{false};
        mutable fleece::Doc             _bodyFleece;
    };

    /** An HTTP response from a server, created by specifying a request to send.
        I.e. this is a simple HTTP client API. */
    class Response : public Body {
      public:
        explicit Response(const net::Address&, net::Method = net::GET);

        Response(const std::string& scheme, const std::string& method, const std::string& hostname, uint16_t port,
                 const std::string& uri);

        Response(const std::string& method, const std::string& hostname, uint16_t port, const std::string& uri)
            : Response("http", method, hostname, port, uri) {}

        ~Response();

        Response& setHeaders(const fleece::Doc& headers);
        Response& setHeaders(const websocket::Headers& headers);

        Response& setAuthHeader(fleece::slice authHeader);
        Response& setBody(fleece::slice body);
        Response& setTLSContext(net::TLSContext*);
        Response& setProxy(const net::ProxySpec&);

        double getTimeout() const { return _timeout; }

        Response& setTimeout(double timeoutSecs) {
            _timeout = timeoutSecs;
            return *this;
        }

        Response& allowOnlyCert(fleece::slice certData);
        Response& setRootCerts(fleece::slice certsData);
#ifdef COUCHBASE_ENTERPRISE
        Response& allowOnlyCert(C4Cert*);
        Response& setRootCerts(C4Cert*);
        Response& setIdentity(C4Cert*, C4KeyPair*);
#endif

        bool run();

        explicit operator bool() { return run(); }

        C4Error error() {
            run();
            return _error;
        }

        HTTPStatus status() {
            run();
            return _status;
        }

        std::string statusMessage() {
            run();
            return _statusMessage;
        }

      protected:
        net::TLSContext* tlsContext();

        bool hasRun() { return _logic == nullptr; }

        void setStatus(int status, const std::string& msg) {
            _status        = (HTTPStatus)status;
            _statusMessage = msg;
        }

      private:
        double                            _timeout{0};
        std::unique_ptr<net::HTTPLogic>   _logic;
        fleece::Retained<net::TLSContext> _tlsContext;
        fleece::alloc_slice               _requestBody;
        HTTPStatus                        _status{HTTPStatus::undefined};
        std::string                       _statusMessage;
        C4Error                           _error{};
    };

}  // namespace litecore::REST
