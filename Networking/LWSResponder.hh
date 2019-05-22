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

    /** Represents an _incoming_ HTTP request received by a LWSServer,
        and the response to the request. */
    class LWSResponder : public websocket::LWSProtocol, public Request {
    public:

        /** Initialize on a new incoming connection. Will read the incoming request,
            then call LWSServer::dispatchResponder with itself as the parameter. */
        LWSResponder(websocket::LWSServer*, lws *connection);

        virtual const char *className() const noexcept override      {return "LWSResponder";}

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

        void setContentLength(uint64_t length);
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
        virtual ~LWSResponder();
        void dispatch(lws *wsi, int reason, void *user, void *in, size_t len) override;
        void onConnectionError(C4Error error) override;
        Method getMethod();

    private:
        void onRequestBody(fleece::slice);
        void onRequestBodyComplete();
        void onRequestReady(fleece::slice uri);
        void dispatch();
        void sendStatus();
        void sendHeaders();
        void onWriteRequest();

        websocket::LWSServer* _server;
        C4Error _error {};

        std::vector<fleece::alloc_slice> _requestBody;

        HTTPStatus _status {HTTPStatus::OK};        // Response status code
        std::string _statusMessage;                 // Response custom status message
        bool _sentStatus {false};                   // Sent the response line yet?

        int64_t _contentLength {-1};                // Content-Length, once it's set
        fleece::alloc_slice _responseHeaders;       // Buffer where LWS writes response headers
        uint8_t *_responseHeadersPos {nullptr};     // Current pos in buffer

        fleece::Writer _responseWriter;             // Output stream for response body
        std::unique_ptr<fleece::JSONEncoder> _jsonEncoder;  // Used for writing JSON to response
        fleece::alloc_slice _responseBody;          // Finished response body
        fleece::slice _unsentBody;                  // Unsent portion of _responseBody
        bool _finished {false};                     // Finished configuring the response?

    };

} }
