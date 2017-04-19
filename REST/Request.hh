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
#include <functional>
#include <memory>
#include <sstream>

struct mg_connection;

namespace litecore { namespace REST {
    class Server;

    /** HTTP request + response */
    class Request {
    public:
        Server* server() const                              {return _server;}
        
        const char* operator[] (const char *header) const;

        const char* path() const;
        fleece::slice path(int i) const;

        std::string query(const char *param) const;
        int64_t intQuery(const char *param, int64_t defaultValue =0) const;
        bool boolQuery(const char *param, bool defaultValue =false) const;

        void respondWithError(int status, const char *message =nullptr);

        void setStatus(unsigned status, const char *message);

        unsigned status() const                             {return _status;}

        void setHeader(const char *header, const char *value);

        void setHeader(const char *header, int64_t value) {
            setHeader(header, std::to_string(value).c_str());
        }

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
