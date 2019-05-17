//
// LWSResponder.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "LWSProtocol.hh"
#include "Request.hh"
#include "Writer.hh"
#include <map>

namespace litecore { namespace websocket {
    class LWSServer;
} }

namespace litecore { namespace REST {

    /** Manages the server side of a connection. */
    class LWSResponder : public websocket::LWSProtocol, public Request {
    public:

        LWSResponder(websocket::LWSServer*, lws *connection);

        // Response status:

        void respondWithStatus(HTTPStatus, const char *message =nullptr);
        void respondWithError(C4Error);

        void setStatus(HTTPStatus status, const char *message);

        HTTPStatus status() const                             {return _status;}

        HTTPStatus errorToStatus(C4Error);

        // Response headers:

        void setHeader(const char *header, const char *value);

        void setHeader(const char *header, int64_t value) {
            setHeader(header, std::to_string(value).c_str());
        }

        void addHeaders(std::map<std::string, std::string>);

        // Response body:

        // If you call write() more than once, you must first call setContentLength or setChunked.
        void setContentLength(uint64_t length);
        void setChunked();
        void uncacheable();

        void write(fleece::slice);
        void write(const char *content)                     {write(fleece::slice(content));}
        void printf(const char *format, ...) __printflike(2, 3);

        fleece::JSONEncoder& jsonEncoder();

        void writeStatusJSON(HTTPStatus status, const char *message =nullptr);
        void writeErrorJSON(C4Error);

        // Must be called after everything's written:
        void finish();

    protected:
        int dispatch(lws *wsi, int reason, void *user, void *in, size_t len) override;
        void onConnectionError(C4Error error) override;

        Method getMethod();

    private:
        bool onRequestReady(fleece::slice uri);
        bool sendHeaders();
        bool onWriteRequest();

        websocket::LWSServer* _server;
        Method _requestMethod;
        fleece::alloc_slice _requestURI;
        fleece::Doc _requestHeaders;

        fleece::alloc_slice _responseHeaders;
        uint8_t *_responseHeadersPos {nullptr};

        fleece::Writer _responseData;
        fleece::alloc_slice _responseBody;
        fleece::slice _unsentBody;
        C4Error _error {};
        std::condition_variable _condition;
        bool _finished {false};

        HTTPStatus _status {HTTPStatus::OK};
        std::string _statusMessage;
        bool _sentStatus {false};
        bool _chunked {false};
        int64_t _contentLength {-1};
        int64_t _contentSent {0};
        std::unique_ptr<fleece::JSONEncoder> _jsonEncoder;
    };

} }
