//
//  HTTPTypes.hh
//  LiteCore
//
//  Created by Jens Alfke on 5/23/19.
//  Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include <limits.h>

namespace litecore { namespace REST {

    enum class HTTPStatus : int {
        undefined = -1,
        OK = 200,
        Created = 201,
        NoContent = 204,
        NotModified = 304,
        BadRequest = 400,
        Unauthorized = 401,
        Forbidden = 403,
        NotFound = 404,
        MethodNotAllowed = 405,
        NotAcceptable = 406,
        Conflict = 409,
        Gone = 410,
        PreconditionFailed = 412,
        Locked = 423,
        ServerError = 500,
        NotImplemented = 501,
        GatewayError = 502,
    };

    const char* StatusMessage(HTTPStatus);


    enum Method: unsigned {
        None        = 0,

        GET         = 1,
        PUT         = 2,
        DELETE      = 4,
        POST        = 8,
        OPTIONS     = 16,

        UPGRADE     = 32,       // represents a WebSocket upgrade request

        ALL         = UINT_MAX
    };

    using Methods = Method;

    const char* MethodName(Method);

} }
