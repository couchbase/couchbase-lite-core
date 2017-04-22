//
//  Request.hh
//  LiteCore
//
//  Created by Jens Alfke on 4/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Response.hh"
#include "PlatformCompat.hh"

namespace litecore { namespace REST {

    /** Incoming HTTP request, with methods for composing a response */
    class Request : public Body {
    public:
        fleece::slice method() const;

        fleece::slice path() const;
        std::string path(int i) const;

        std::string query(const char *param) const;
        int64_t intQuery(const char *param, int64_t defaultValue =0) const;
        bool boolQuery(const char *param, bool defaultValue =false) const;

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

        fleeceapi::JSONEncoder& jsonEncoder();

    protected:
        friend class Server;

        Request(mg_connection*);

        void finish();

    private:
        void sendHeaders();

        unsigned _status {200};
        std::stringstream _headers;
        bool _sentStatus {false};
        bool _sentHeaders {false};
        bool _chunked {false};
        int64_t _contentLength {-1};
        int64_t _contentSent {0};
        std::unique_ptr<fleeceapi::JSONEncoder> _jsonEncoder;
    };

} }
