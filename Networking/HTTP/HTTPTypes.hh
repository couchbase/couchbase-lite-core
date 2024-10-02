//
//  HTTPTypes.hh
//  LiteCore
//
//  Created by Jens Alfke on 5/23/19.
//  Copyright 2019-Present Couchbase, Inc.
//
//  Use of this software is governed by the Business Source License included
//  in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
//  in that file, in accordance with the Business Source License, use of this
//  software will be governed by the Apache License, Version 2.0, included in
//  the file licenses/APL2.txt.
//

#pragma once
#include <climits>
#include "Address.hh"
#include "fleece/slice.hh"

#ifdef _MSC_VER
#    pragma push_macro("DELETE")
#    undef DELETE
#endif

namespace litecore::net {

    /// HTTP status codes
    enum class HTTPStatus : int {
        undefined = -1,
        Upgraded  = 101,

        OK        = 200,
        Created   = 201,
        NoContent = 204,

        MovedPermanently  = 301,
        Found             = 302,
        SeeOther          = 303,
        NotModified       = 304,
        UseProxy          = 305,
        TemporaryRedirect = 307,

        BadRequest          = 400,
        Unauthorized        = 401,
        Forbidden           = 403,
        NotFound            = 404,
        MethodNotAllowed    = 405,
        NotAcceptable       = 406,
        ProxyAuthRequired   = 407,
        Conflict            = 409,
        Gone                = 410,
        PreconditionFailed  = 412,
        UnprocessableEntity = 422,
        Locked              = 423,

        ServerError    = 500,
        NotImplemented = 501,
        GatewayError   = 502,
    };

    inline bool IsSuccess(HTTPStatus s) { return int(s) < 300; }

    const char* StatusMessage(HTTPStatus);

    /// HTTP methods. These do NOT have consecutive values, rather they're powers of two
    /// so they can be used as bit-masks.
    enum Method : unsigned {
        None = 0,

        HEAD    = 1,
        GET     = 2,
        PUT     = 4,
        DELETE  = 8,
        POST    = 16,
        OPTIONS = 32,

        UPGRADE = 64,  // represents a WebSocket upgrade request

        ALL = UINT_MAX
    };

    /// A set of Methods encoded as bits.
    using Methods = Method;

    const char* MethodName(Method);
    Method      MethodNamed(fleece::slice name);


    /// Types of proxy servers.
    enum class ProxyType {
        HTTP,
        HTTPS,
        //SOCKS,      // TODO: Add SOCKS support
    };

    /// Proxy configuration, used by HTTPLogic.
    struct ProxySpec {
        ProxyType           type;
        fleece::alloc_slice hostname;
        uint16_t            port;
        fleece::alloc_slice username;
        fleece::alloc_slice password;

        explicit ProxySpec(const C4Address&);

        ProxySpec(ProxyType t, fleece::slice host, uint16_t port_) : type(t), hostname(host), port(port_) {}

        ProxySpec(ProxyType t, const C4Address& a) : type(t), hostname(a.hostname), port(a.port) {}

        explicit operator Address() const;
    };

}  // namespace litecore::net

#ifdef _MSC_VER
#    pragma pop_macro("DELETE")
#endif
