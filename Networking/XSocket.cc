//
// XSocket.cc
//
// Copyright © 2019 Couchbase. All rights reserved.
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

#include "XSocket.hh"
#include "Headers.hh"
#include "HTTPLogic.hh"
#include "WebSocketInterface.hh"
#include "Error.hh"
#include "SecureDigest.hh"
#include "SecureRandomize.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "sockpp/exception.h"
#include "sockpp/tcp_connector.h"
#include "sockpp/tls_socket.h"
#include "make_unique.h"
#include <netdb.h>
#include <regex>
#include <string>
#include <sstream>

namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;
    using namespace sockpp;
    using namespace litecore::websocket;


    static constexpr size_t kInitialDelimitedReadBufferSize = 1024;


    XSocket::XSocket(tls_context *tls)
    :_tlsContext(tls)
    { }


    XSocket::~XSocket()
    { }


    tls_context* XSocket::TLSContext() {
        return _tlsContext;
    }


    void XSocket::checkSocketFailure() {
        if (*_socket)
            return;

        // TLS handshake failed:
        if (_socket->last_error() == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            // Some more specific errors for certificate validation failures, based on flags:
            auto tlsSocket = (tls_socket*)_socket.get();
            uint32_t flags = tlsSocket->peer_certificate_status();
            LogToAt(websocket::WSLogDomain, Warning, "XSocket TLS handshake failed; cert verify status 0x%02x", flags);
            if (flags != 0 && flags != UINT32_MAX) {
                string message = tlsSocket->peer_certificate_status_message();
                int code = kNetErrTLSCertUntrusted;
                if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
                    code = kNetErrTLSCertUnknownRoot;
                else if (flags & MBEDTLS_X509_BADCERT_REVOKED)
                    code = kNetErrTLSCertRevoked;
                else if (flags & MBEDTLS_X509_BADCERT_EXPIRED)
                    code = kNetErrTLSCertExpired;
                else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
                    code = kNetErrTLSCertNameMismatch;
                error{error::Network, code, message}._throw();
            }
        }

        _throwLastError();
    }


    bool XSocket::connected() const {
        return _socket && _socket->is_open();
    }


    void XSocket::close() {
        if (_socket) {
            _socket->close();
        }
    }


    size_t XSocket::write(slice data) {
        if (data.size == 0)
            return 0;
        ssize_t written = _socket->write(data.buf, data.size);
        if (written < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            _throwLastError();
        }
        return size_t(written);
    }


    size_t XSocket::write_n(slice data) {
        if (data.size == 0)
            return 0;
        ssize_t written = _socket->write_n(data.buf, data.size);
        if (written < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            _throwLastError();
        }
        return size_t(written);
    }


    // Primitive unbuffered read call. If an error occurs, throws an exception. Returns 0 on EOF.
    size_t XSocket::_read(void *dst, size_t byteCount) {
        ssize_t n = _socket->read(dst, byteCount);
        if (n < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            _throwLastError();
        }
        return n;
    }


    // "Un-read" data by prepending it to the _unread buffer
    void XSocket::pushUnread(slice data) {
        if (_usuallyFalse(data.size == 0))
            return;
        if (_usuallyTrue(_unreadLen + data.size > _unread.size))
            _unread.resize(_unreadLen + data.size);
        memcpy((void*)&_unread[data.size], &_unread[0], _unreadLen);
        memcpy((void*)&_unread[0], data.buf, data.size);
        _unreadLen += data.size;
    }


    // Read from the socket, or from the unread buffer if it exists
    size_t XSocket::read(void *dst, size_t byteCount) {
        if (_usuallyFalse(_unreadLen > 0)) {
            // Use up anything left in the buffer:
            size_t n = min(byteCount, _unreadLen);
            memcpy(dst, &_unread[0], n);
            memmove((void*)&_unread[0], &_unread[n], _unreadLen - n);
            _unreadLen -= n;
            if (_unreadLen == 0)
                _unread = nullslice;
            return n;
        } else {
            return _read(dst, byteCount);
        }
    }


    // Read exactly `byteCount` bytes from the socket (or the unread buffer)
    void XSocket::readExactly(void *dst, size_t byteCount) {
        while (byteCount > 0) {
            auto n = read(dst, byteCount);
            if (n == 0)
                error::_throw(error::WebSocket, 400);  // unexpected EOF
            byteCount -= n;
            dst = offsetby(dst, n);
        }
    }


    // Read up to the given delimiter.
    alloc_slice XSocket::readToDelimiter(slice delim, bool includeDelim, size_t maxSize) {
        alloc_slice alloced(kInitialDelimitedReadBufferSize);
        slice result(alloced.buf, size_t(0));

        while (true) {
            // Read more bytes:
            ssize_t n = _read((void*)result.end(), alloced.size - result.size);
            if (n == 0)
                error::_throw(error::WebSocket, 400);
            result.setSize(result.size + n);

            // Look for delimiter:
            slice found = result.find(delim);
            if (found) {
                pushUnread(slice(found.end(), result.end()));
                result.setEnd(found.end());
                alloced.resize(result.size);
                return alloced;
            }

            // If allocated buffer is full, grow it:
            if (result.size == alloced.size) {
                size_t newSize = min(alloced.size * 2, maxSize);
                if (newSize == alloced.size)
                    error::_throw(error::WebSocket, 431);
                alloced.resize(newSize);
                result.setStart(alloced.buf);
            }
        }
    }


    alloc_slice XSocket::readHTTPBody(const Headers &headers) {
        alloc_slice body;
        int64_t contentLength = headers.getInt("Content-Length"_sl, -1);
        if (contentLength >= 0) {
            // Read exactly Content-Length bytes:
            if (contentLength > 0) {
                body.resize(contentLength);
                readExactly((void*)body.buf, (size_t)contentLength);
            }
        } else {
            // No Content-Length, so read till EOF:
            body.resize(1024);
            size_t length = 0;
            while (true) {
                size_t n = read((void*)&body[length], body.size - length);
                if (n == 0)
                    break;
                length += n;
                if (length == body.size)
                    body.resize(2 * body.size);
            }
            body.resize(length);
        }
        return body;
    }


#pragma mark - ERRORS:


    int XSocket::mbedToNetworkErrCode(int err) {
        static constexpr struct {int mbed0; int mbed1; int net;} kMbedToNetErr[] = {
            {MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, kNetErrTLSCertUntrusted},
            {-0x3000,                             -0x2000,                             kNetErrTLSCertUntrusted},
            {-0x7FFF,                             -0x6000,                             kNetErrTLSHandshakeFailed},
            {0, 0, 0}
        };
        for (int i = 0; kMbedToNetErr[i].mbed0 != 0; ++i) {
            if (kMbedToNetErr[i].mbed0 <= err && err <= kMbedToNetErr[i].mbed1)
                return kMbedToNetErr[i].net;
        }
        Warn("No mapping for mbedTLS error -0x%04X", -err);
        return kNetErrUnknown;
    }


    void XSocket::_throwLastError() {
        int err = _socket->last_error();
        Assert(err != 0);
        if (err > 0) {
            LogToAt(websocket::WSLogDomain, Warning,
                    "XSocket got POSIX error %d; throwing exception...", err);
            error::_throw(error::POSIX, err);
        } else {
            // Negative errors are assumed to be from mbedTLS.
            char msgbuf[100];
            mbedtls_strerror(err, msgbuf, sizeof(msgbuf));
            LogToAt(websocket::WSLogDomain, Warning,
                    "XSocket got mbedTLS error -0x%04X \"%s\"; throwing exception...", -err, msgbuf);
            int netErr = mbedToNetworkErrCode(err);
            error(error::Network, netErr, msgbuf)._throw();
        }
    }


    void XSocket::_throwBadHTTP() {
        error{error::WebSocket, 400, "Received invalid HTTP response"}._throw();
    }


    error XSocket::convertException(const std::exception &x) {
        error::Domain domain;
        int code;
        const char *message = x.what();
        string messagebuf;
        auto sx = dynamic_cast<const sockpp::sys_error*>(&x);
        if (sx) {
            // sockpp 'errno' exception:
            domain = error::POSIX;
            code = sx->error();
        } else {
            auto gx = dynamic_cast<const sockpp::getaddrinfo_error*>(&x);
            if (gx) {
                // sockpp 'getaddrinfo' exception:
                domain = error::Network;
                if (gx->error() == EAI_NONAME || gx->error() == HOST_NOT_FOUND) {
                    code = kC4NetErrUnknownHost;
                    messagebuf = "Unknown hostname " + gx->hostname();
                } else {
                    code = kC4NetErrDNSFailure;
                    messagebuf = "Error resolving hostname " + gx->hostname() + ": " + gx->what();
                }
                message = messagebuf.c_str();
            } else {
                // Not a sockpp exception, so let error class handle it:
                return error::convertException(x);
            }
        }
        return error(domain, code, message);
    }


#pragma mark - CLIENT SOCKET:


    XClientSocket::XClientSocket(tls_context *tls)
    :XSocket(tls)
    { }


    void XClientSocket::connect(const repl::Address &addr) {
        string hostname(slice(addr.hostname));
        auto socket = make_unique<tcp_connector>(inet_address{hostname, addr.port});
        if (addr.isSecure() && *socket) {
            if (!_tlsContext)
                _tlsContext = _tlsContext = &tls_context::default_context();
            _socket = TLSContext()->wrap_socket(move(socket), tls_context::CLIENT, hostname);
        } else {
            _socket = move(socket);
        }
        checkSocketFailure();
    }


#pragma mark - RESPONDER SOCKET:


    XResponderSocket::XResponderSocket(tls_context *tls)
    :XSocket(tls)
    { }


    void XResponderSocket::acceptSocket(stream_socket &&s, bool useTLS) {
        acceptSocket( make_unique<tcp_socket>(move(s)), useTLS );
    }

    
    void XResponderSocket::acceptSocket(unique_ptr<stream_socket> socket, bool useTLS) {
        if (_tlsContext)
            _socket = _tlsContext->wrap_socket(move(socket), tls_context::SERVER, "");
        else
            _socket = move(socket);
        checkSocketFailure();
    }

} }
