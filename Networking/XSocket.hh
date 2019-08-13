//
// XSocket.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "RefCounted.hh"
#include "Address.hh"
#include "function_ref.hh"
#include "fleece/Fleece.hh"
#include "sockpp/tcp_connector.h"
#include <memory>
#include <thread>

namespace litecore { namespace websocket {
    struct CloseStatus;
} }

namespace litecore { namespace net {

    class XSocket {
    public:
        using slice = fleece::slice;
        using string = std::string;

        XSocket(repl::Address addr)
        :_addr(addr)
        { }

        /// Connects to the host, synchronously. On failure throws an exception.
        void connect();

        /// Closes the socket if it's open.
        void close();

        /// Sends an HTTP request, but not a body.
        void sendHTTPRequest(const string &method,
                             fleece::function_ref<void(std::stringstream&)>);

        struct Response {
            int status;
            string message;
            fleece::AllocedDict headers;
        };

        /// Reads an HTTP response, but not the body. On failure throws an exception.
        Response readHTTPResponse();

        /// Reads an HTTP body given the headers.
        /// If there's a Content-Length header, reads that many bytes.
        /// Otherwise reads till EOF.
        fleece::alloc_slice readHTTPBody(fleece::AllocedDict headers);

        /// Sends a WebSocket handshake request.
        string sendWebSocketRequest(fleece::Dict headers, const string &protocol);

        /// Reads a WebSocket handshake response.
        /// On failure returns false and stores the details in \ref outStatus.
        bool checkWebSocketResponse(const Response&,
                                    const string &nonce,
                                    const string &requiredProtocol,
                                    websocket::CloseStatus &outStatus);

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
        bool getIntHeader(fleece::Dict headers, slice key, int64_t &value);
        [[noreturn]] void _throwLastError();
        size_t _read(void *dst, size_t byteCount);

    private:
        static constexpr size_t kReadBufferSize = 8192;
        
        repl::Address _addr;
        std::unique_ptr<sockpp::tcp_connector> _socket;
        std::thread _reader;
        std::thread _writer;

        uint8_t _readBuffer[kReadBufferSize];
        slice const _input = {_readBuffer, sizeof(_readBuffer)};
        uint8_t *_inputStart = (uint8_t*)_input.buf;
        size_t _inputLen = 0;
    };


} }
