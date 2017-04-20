//
//  Request.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include "FleeceCpp.hh"
#include "c4Base.h"
#include <functional>
#include <map>
#include <memory>
#include <sstream>

struct mg_connection;

namespace litecore { namespace REST {
    class Server;

    /** HTTP request + response */
    class Request {
    public:
        Server* server() const                              {return _server;}

        fleece::slice method() const;

        fleece::slice header(const char *name) const;
        fleece::slice operator[] (const char *name) const   {return header(name);}

        fleece::slice path() const;
        fleece::slice path(int i) const;

        std::string query(const char *param) const;
        int64_t intQuery(const char *param, int64_t defaultValue =0) const;
        bool boolQuery(const char *param, bool defaultValue =false) const;

        bool hasContentType(fleece::slice contentType) const;
        fleece::alloc_slice requestBody() const;
        fleeceapi::Value requestJSON() const;

        // RESPONSE:

        void respondWithError(int status, const char *message =nullptr);
        void respondWithError(C4Error);

        void setStatus(unsigned status, const char *message);

        unsigned status() const                             {return _status;}

        void setHeader(const char *header, const char *value);

        void setHeader(const char *header, int64_t value) {
            setHeader(header, std::to_string(value).c_str());
        }

        void addHeaders(std::map<std::string, std::string>);

        // If you call write() more than once, you must first call setContentLength or setChunked.
        void setContentLength(uint64_t length);
        void setChunked();

        void write(fleece::slice);
        void write(const char *content)                     {write(fleece::slice(content));}
        void printf(const char *format, ...) __printflike(2, 3);

        fleeceapi::JSONEncoder& json();

        // Utilities:
        static std::string urlDecode(const std::string&);
        static std::string urlEncode(const std::string&);

    protected:
        friend class Server;

        Request(Server*, mg_connection*);

        void finish();

    private:
        void sendHeaders();

        Server* const _server;
        mg_connection* const _conn;
        // Request stuff:
        bool _gotRequestBody {false};
        fleece::alloc_slice _requestBody;
        bool _gotRequestBodyFleece {false};
        fleece::alloc_slice _requestBodyFleece;
        // Response stuff:
        unsigned _status {200};
        std::stringstream _headers;
        bool _sentStatus {false};
        bool _sentHeaders {false};
        bool _chunked {false};
        int64_t _contentLength {-1};
        int64_t _contentSent {0};
        std::unique_ptr<fleeceapi::JSONEncoder> _json;
    };

} }
