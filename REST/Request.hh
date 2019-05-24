//
// Request.hh
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
#include "Response.hh"
#include "HTTPTypes.hh"
#include "LWSResponder.hh"
#include "PlatformCompat.hh"
#include "StringUtil.hh"

namespace litecore { namespace REST {
    class Server;

    /** Incoming HTTP request; read-only */
    class Request : public Body {
    public:
        Method method() const                   {return _method;}

        std::string path() const                {return _path;}
        std::string path(int i) const;

        std::string query(const char *param) const;
        int64_t intQuery(const char *param, int64_t defaultValue =0) const;
        bool boolQuery(const char *param, bool defaultValue =false) const;

    protected:
        friend class Server;
        
        Request(Method, std::string path, fleece::slice queries,
                fleece::Doc headers, fleece::alloc_slice body);
        Request() { }

        Method _method {Method::None};
        std::string _path;
        fleece::alloc_slice _queries;
    };


    /** Incoming HTTP request (inherited from Request),
        with setters to send the response (inherited from LWSResponder). */
    class RequestResponse : public Request, public net::LWSResponder {
    protected:
        RequestResponse(Server *server, lws *client);
        virtual void onRequest(Method,
                               std::string path,
                               fleece::slice queries,
                               fleece::Doc headers) override;
        virtual void onRequestBody(fleece::alloc_slice) override;

        friend class Server;
    };

} }
