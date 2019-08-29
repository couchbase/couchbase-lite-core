//
// TCPSocket.hh
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
    class Headers;
} }
namespace sockpp {
    class stream_socket;
    class tls_context;
}

namespace litecore { namespace net {
    class HTTPLogic;

    /** TCP socket class, using the sockpp library. */
    class TCPSocket {
    public:
        using slice = fleece::slice;

        TCPSocket(sockpp::tls_context *ctx =nullptr);
        virtual ~TCPSocket();

        /// Initializes TCPSocket, must call at least once before using any
        /// socket related functionality
        static void initialize();

        /// Returns the TLS context, if any, used by this socket.
        sockpp::tls_context* TLSContext();

        /// Closes the socket if it's open.
        void close();

        bool connected() const;
        operator bool() const                   {return connected();}

        C4Error error() const                   {return _error;}

        /// Reads up to \ref byteCount bytes to the location \ref dst.
        /// On EOF returns zero. On other error returns -1.
        ssize_t read(void *dst, size_t byteCount) MUST_USE_RESULT;

        /// Reads exactly \ref byteCount bytes to the location \ref dst.
        /// On premature EOF returns 0 and sets error {WebSocket, 400}.
        ssize_t readExactly(void *dst, size_t byteCount) MUST_USE_RESULT;

        static constexpr size_t kMaxDelimitedReadSize = 50 * 1024;

        /// Reads from the socket until the \ref delimiter byte sequence is found,
        /// and returns the bytes read ending with the delimiter.
        /// If the delimiter is not found, due to EOF of reading more than \ref maxSize bytes,
        /// throws an exception.
        fleece::alloc_slice readToDelimiter(slice delimiter,
                                            bool includeDelimiter =true,
                                            size_t maxSize =kMaxDelimitedReadSize) MUST_USE_RESULT;

        /// Reads an HTTP body, given the headers.
        /// If there's a Content-Length header, reads that many bytes, otherwise reads till EOF.
        bool readHTTPBody(const websocket::Headers &headers, fleece::alloc_slice &body) MUST_USE_RESULT;

        /// Writes to the socket and returns the number of bytes written:
        ssize_t write(slice) MUST_USE_RESULT;

        /// Writes all the bytes to the socket.
        ssize_t write_n(slice) MUST_USE_RESULT;

    protected:
        void setError(C4ErrorDomain, int code, slice message);
        bool wrapTLS(slice hostname, bool isClient);
        static int mbedToNetworkErrCode(int mbedErr);
        void checkStreamError();
        bool checkSocketFailure();
        ssize_t _read(void *dst, size_t byteCount) MUST_USE_RESULT;
        void pushUnread(slice);

        std::unique_ptr<sockpp::stream_socket> _socket;
        sockpp::tls_context* _tlsContext = nullptr;

    private:
        static constexpr size_t kReadBufferSize = 8192;
        
        std::thread _reader;
        std::thread _writer;

        C4Error _error {};
        fleece::alloc_slice _unread;        // Data read from socket that's been "pushed back"
        size_t _unreadLen {0};              // Length of valid data in _unread
    };



    /** A client socket, that opens a TCP connection. */
    class ClientSocket : public TCPSocket {
    public:
        ClientSocket(sockpp::tls_context* =nullptr);

        /// Connects to the host, synchronously. On failure throws an exception.
        bool connect(const Address &addr) MUST_USE_RESULT;

        /// Wrap the existing socket in TLS, performing a handshake.
        /// This is used after connecting to a CONNECT-type proxy, not in a normal connection.
        bool wrapTLS(slice hostname)        {return TCPSocket::wrapTLS(hostname, true);}
    };

    

    /** A server-side socket, that handles a client connection. */
    class ResponderSocket : public TCPSocket {
    public:
        ResponderSocket(sockpp::tls_context* =nullptr);

        bool acceptSocket(sockpp::stream_socket&&) MUST_USE_RESULT;
        bool acceptSocket(std::unique_ptr<sockpp::stream_socket>) MUST_USE_RESULT;

        /// Perform server-side TLS handshake.
        bool wrapTLS()                      {return TCPSocket::wrapTLS(fleece::nullslice, false);}
    };


} }
