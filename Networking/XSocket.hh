//
// XSocket.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "Address.hh"
#include "HTTPTypes.hh"
#include "function_ref.hh"
#include "fleece/Fleece.hh"
#include <memory>
#include <thread>

namespace litecore { namespace websocket {
    struct CloseStatus;
} }

namespace sockpp {
    class stream_socket;
    class tls_context;
}

namespace litecore { namespace net {

    /** TCP client socket class, using the sockpp library. */
    class XSocket {
    public:
        using slice = fleece::slice;
        using string = std::string;

        XSocket();
        virtual ~XSocket();

        /// Associates this socket with a TLS context.
        void setTLSContext(sockpp::tls_context*);

        /// Returns the TLS context, if any, used by this socket.
        sockpp::tls_context* TLSContext();

        /// Closes the socket if it's open.
        void close();

        bool connected() const;
        operator bool() const   {return connected();}

        /// Reads an HTTP body given the headers.
        /// If there's a Content-Length header, reads that many bytes.
        /// Otherwise reads till EOF.
        fleece::alloc_slice readHTTPBody(fleece::AllocedDict headers);

        /// Reads up to \ref byteCount bytes to the location \ref dst.
        /// On EOF returns zero. On other error throws an exception.
        size_t read(void *dst, size_t byteCount);

        /// Reads exactly \ref byteCount bytes to the location \ref dst.
        /// On EOF returns zero. On other error throws an exception.
        void readExactly(void *dst, size_t byteCount);

        /// Reads into the internal buffer and returns a pointer to the read data
        /// On EOF returns \ref nullslice. On other error throws an exception.
        slice read(size_t byteCount =kReadBufferSize);

        /// Reads into the internal buffer until the \ref delimiter byte sequence is found,
        /// and returns the bytes read ending with the delimiter.
        /// If the buffer fills up before the delimiter is found, or EOF is reached,
        /// returns nullslice.
        /// On error throws an exception.
        /// This method is likely to read bytes past the delimiter! The extra bytes will be
        /// returned by subsequent reads.
        slice readToDelimiter(slice delimiter);

        /// Writes to the socket and returns the number of bytes written:
            __attribute__((warn_unused_result))
        size_t write(slice);

        // Writes all the bytes to the socket.
        size_t write_n(slice);

    protected:
        [[noreturn]] void _throwLastError();
        [[noreturn]] void _throwBadHTTP();
        void checkSocketFailure();
        size_t _read(void *dst, size_t byteCount);
        fleece::AllocedDict readHeaders();
        void writeHeaders(std::stringstream &rq, fleece::Dict headers);
        bool getIntHeader(fleece::Dict headers, slice key, int64_t &value);

        std::unique_ptr<sockpp::stream_socket> _socket;
        sockpp::tls_context* _tlsContext = nullptr;

        // What I'm currently reading/writing:
        enum state {
            kRequestLine,
            kStatusLine = kRequestLine,
            kHeaders,
            kBody,
            kEnd
        };
        state _writeState = kStatusLine, _readState = kStatusLine;

    private:
        static constexpr size_t kReadBufferSize = 8192;
        
        std::thread _reader;
        std::thread _writer;

        uint8_t _readBuffer[kReadBufferSize];
        slice const _input = {_readBuffer, sizeof(_readBuffer)};
        uint8_t *_inputStart = (uint8_t*)_input.buf;
        size_t _inputLen = 0;
    };



    class HTTPClientSocket : public XSocket {
    public:
        HTTPClientSocket(repl::Address addr);

        /// Connects to the host, synchronously. On failure throws an exception.
        void connect();

        /// Sends an HTTP request, but not a body.
        void sendHTTPRequest(const std::string &method,
                             fleece::Dict headers,
                             fleece::slice body =fleece::nullslice);

        struct HTTPResponse {
            REST::HTTPStatus status;
            string message;
            fleece::AllocedDict headers;
        };

        /// Reads an HTTP response, but not the body. On failure throws an exception.
        HTTPResponse readHTTPResponse();

        /// Sends a WebSocket handshake request.
        string sendWebSocketRequest(fleece::Dict headers, const string &protocol);

        /// Reads a WebSocket handshake response.
        /// On failure returns false and stores the details in \ref outStatus.
        bool checkWebSocketResponse(const HTTPResponse&,
                                    const string &nonce,
                                    const string &requiredProtocol,
                                    websocket::CloseStatus &outStatus);
    private:
        void sendHTTPRequest(const string &method,
                             fleece::function_ref<void(std::stringstream&)>);

        repl::Address _addr;
    };

    

    class HTTPResponderSocket : public XSocket {
    public:
        void acceptSocket(sockpp::stream_socket&&, bool useTLS =false);
        void acceptSocket(std::unique_ptr<sockpp::stream_socket>, bool useTLS =false);

        struct HTTPRequest {
            REST::Method method;
            string path, query;
            fleece::AllocedDict headers;
        };

        HTTPRequest readHTTPRequest();

        void writeResponseLine(REST::HTTPStatus, slice message);
        void writeHeader(slice name, slice value);
        void endHeaders();
    };


} }
