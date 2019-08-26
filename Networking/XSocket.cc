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


    size_t XSocket::read(void *dst, size_t byteCount) {
        if (_inputLen > 0) {
            // Use up anything left in the buffer:
            size_t n = min(byteCount, _inputLen);
            memmove(dst, _inputStart, n);
            _inputLen -= n;
            _inputStart += n;
            return n;
        } else {
            return _read(dst, byteCount);
        }
    }

    void XSocket::readExactly(void *dst, size_t byteCount) {
        while (byteCount > 0) {
            auto n = read(dst, byteCount);
            if (n == 0)
                error::_throw(error::WebSocket, 599);  // unexpected EOF // TODO: error code
            byteCount -= n;
            dst = offsetby(dst, n);
        }
    }

    // Read into the internal buffer
    slice XSocket::read(size_t byteCount) {
        if (_inputLen > 0) {
            // Use up anything left in the buffer
            const void *dst = _inputStart;
            return slice(dst, read(_inputStart, byteCount));
        } else {
            size_t n = _read((void*)_input.buf, min(_input.size, byteCount));
            if (n == 0)
                return nullslice;
            return {_input.buf, n};
        }
    }


    slice XSocket::readToDelimiter(slice delim, bool includeDelim) {
        if (_inputLen > 0 && _inputStart > _input.buf) {
            // Slide unread input down to start of buffer:
            memmove((void*)_input.buf, _inputStart, _inputLen);
            _inputStart = (uint8_t*)_input.buf;
        }

        while (true) {
            // Look for delimiter:
            slice found = slice(_inputStart, _inputLen).find(delim);
            if (found) {
                slice result(_inputStart, (includeDelim ? found.end() : found.buf));
                auto inputEnd = _inputStart + _inputLen;
                _inputStart = (uint8_t*)found.end();
                _inputLen = inputEnd - _inputStart;
                return result;
            }

            // Give up if buffer is full:
            if (_inputLen >= _input.size)
                return nullslice; // give up

            // Read more bytes:
            ssize_t n = _read(_inputStart + _inputLen, _input.size - _inputLen);
            if (n == 0)
                return nullslice;
            _inputLen += n;
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
