//
// TCPSocket.cc
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

#include "TCPSocket.hh"
#include "Headers.hh"
#include "HTTPLogic.hh"
#include "WebSocketInterface.hh"
#include "SecureDigest.hh"
#include "SecureRandomize.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "sockpp/exception.h"
#include "sockpp/tcp_connector.h"
#include "sockpp/tls_socket.h"
#include "make_unique.h"
#include <regex>
#include <string>
#include <sstream>
#include <mutex>

namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;
    using namespace sockpp;
    using namespace litecore::websocket;


    static constexpr size_t kInitialDelimitedReadBufferSize = 1024;

    void TCPSocket::initialize() {
        static once_flag f;
        call_once(f, [=] {
            socket::initialize();
        });
    }


    TCPSocket::TCPSocket(tls_context *tls)
    :_tlsContext(tls) {
        initialize();
    }


    TCPSocket::~TCPSocket()
    { }


    tls_context* TCPSocket::TLSContext() {
        return _tlsContext;
    }


    void TCPSocket::setError(C4ErrorDomain domain, int code, slice message) {
        Assert(code != 0);
        _error = c4error_make(domain, code, message);
    }


    bool TCPSocket::wrapTLS(slice hostname, bool isClient) {
        if (!_tlsContext)
            _tlsContext = _tlsContext = &tls_context::default_context();
        string hostnameStr(hostname);
        auto oldSocket = move(_socket);
        _socket = TLSContext()->wrap_socket(move(oldSocket),
                                            (isClient ? tls_context::CLIENT : tls_context::SERVER),
                                            hostnameStr.c_str());
        return checkSocketFailure();
    }


    bool TCPSocket::checkSocketFailure() {
        if (*_socket)
            return true;

        // TLS handshake failed:
        if (_socket->last_error() == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            // Some more specific errors for certificate validation failures, based on flags:
            auto tlsSocket = (tls_socket*)_socket.get();
            uint32_t flags = tlsSocket->peer_certificate_status();
            C4LogToAt(kC4WebSocketLog, kC4LogError,
                      "TCPSocket TLS handshake failed; cert verify status 0x%02x", flags);
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
                setError(NetworkDomain, code, slice(message));
            }
        } else {
            checkStreamError();
        }
        return false;
    }


    bool TCPSocket::connected() const {
        return _socket && _socket->is_open();
    }


    void TCPSocket::close() {
        if (_socket) {
            _socket->close();
        }
    }


    ssize_t TCPSocket::write(slice data) {
        if (data.size == 0)
            return 0;
        ssize_t written = _socket->write(data.buf, data.size);
        if (written < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            checkStreamError();
        }
        return written;
    }


    ssize_t TCPSocket::write_n(slice data) {
        if (data.size == 0)
            return 0;
        ssize_t written = _socket->write_n(data.buf, data.size);
        if (written < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            checkStreamError();
        }
        return written;
    }


    // Primitive unbuffered read call. Returns 0 on EOF, -1 on error (and sets _error).
    // Interprets error EBADF (bad file descriptor) as EOF,
    // since that's what happens when another thread closes the socket while read() is blocked
    ssize_t TCPSocket::_read(void *dst, size_t byteCount) {
        ssize_t n = _socket->read(dst, byteCount);
        if (n < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            checkStreamError();
        }
        return n;
    }


    // "Un-read" data by prepending it to the _unread buffer
    void TCPSocket::pushUnread(slice data) {
        if (_usuallyFalse(data.size == 0))
            return;
        if (_usuallyTrue(_unreadLen + data.size > _unread.size))
            _unread.resize(_unreadLen + data.size);
        memcpy((void*)&_unread[data.size], &_unread[0], _unreadLen);
        memcpy((void*)&_unread[0], data.buf, data.size);
        _unreadLen += data.size;
    }


    // Read from the socket, or from the unread buffer if it exists
    ssize_t TCPSocket::read(void *dst, size_t byteCount) {
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
    ssize_t TCPSocket::readExactly(void *dst, size_t byteCount) {
        ssize_t remaining = byteCount;
        while (remaining > 0) {
            auto n = read(dst, remaining);
            if (n < 0)
                return n;
            if (n == 0) {
                setError(WebSocketDomain, 400, "Premature end of HTTP body"_sl);
                return n;
            }
            remaining -= n;
            dst = offsetby(dst, n);
        }
        return byteCount;
    }


    // Read up to the given delimiter.
    alloc_slice TCPSocket::readToDelimiter(slice delim, bool includeDelim, size_t maxSize) {
        alloc_slice alloced(kInitialDelimitedReadBufferSize);
        slice result(alloced.buf, size_t(0));

        while (true) {
            // Read more bytes:
            ssize_t n = _read((void*)result.end(), alloced.size - result.size);
            if (n < 0)
                return nullslice;
            if (n == 0) {
                setError(WebSocketDomain, 400, "Unexpected EOF"_sl);
                return nullslice;
            }
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
                if (newSize == alloced.size) {
                    setError(WebSocketDomain, 431, "Headers too large"_sl);
                    return nullslice;
                }
                alloced.resize(newSize);
                result.setStart(alloced.buf);
            }
        }
    }


    bool TCPSocket::readHTTPBody(const Headers &headers, alloc_slice &body) {
        int64_t contentLength = headers.getInt("Content-Length"_sl, -1);
        if (contentLength >= 0) {
            // Read exactly Content-Length bytes:
            if (contentLength > 0) {
                body.resize(contentLength);
                if (readExactly((void*)body.buf, (size_t)contentLength) < contentLength) {
                    body.reset();
                    return false;
                }
            }
        } else {
            // No Content-Length, so read till EOF:
            body.resize(1024);
            size_t length = 0;
            while (true) {
                size_t n = read((void*)&body[length], body.size - length);
                if (n < 0) {
                    body.reset();
                    return false;
                } else if (n == 0)
                    break;
                length += n;
                if (length == body.size)
                    body.resize(2 * body.size);
            }
            body.resize(length);
        }
        return true;
    }


#pragma mark - ERRORS:

    static int socketToPosixErrCode(int err) {
#ifdef WIN32
        static constexpr struct {long fromErr; int toErr;} kWSAToPosixErr[] = {
            {WSAECONNREFUSED, ECONNREFUSED},
            {WSAEADDRINUSE, EADDRINUSE},
            {WSAEADDRNOTAVAIL, EADDRNOTAVAIL},
            {WSAEAFNOSUPPORT, EAFNOSUPPORT},
            {WSAECONNABORTED, ECONNABORTED},
            {WSAECONNRESET, ECONNRESET},
            {WSAEHOSTUNREACH, EHOSTUNREACH},
            {WSAENETDOWN, ENETDOWN},
            {WSAENETRESET, ENETRESET},
            {WSAENETUNREACH, ENETUNREACH},
            {WSAENOBUFS, ENOBUFS},
            {WSAEISCONN, EISCONN},
            {WSAENOTCONN, ENOTCONN},
            {WSAETIMEDOUT, ETIMEDOUT},
            {WSAELOOP, ELOOP},
            {WSAENAMETOOLONG, ENAMETOOLONG},
            {WSAEACCES, EACCES},
            {WSAEMFILE, EMFILE},
            {WSAEWOULDBLOCK, EWOULDBLOCK},
            {WSAEALREADY, EALREADY},
            {WSAENOTSOCK, ENOTSOCK},
            {WSAEDESTADDRREQ, EDESTADDRREQ},
            {WSAEPROTOTYPE, EPROTOTYPE},
            {WSAENOPROTOOPT, ENOPROTOOPT},
            {WSAEPROTONOSUPPORT, EPROTONOSUPPORT},
            {WSAEPFNOSUPPORT, EPROTONOSUPPORT},
            {0, 0}
        };
        for(int i = 0; kWSAToPosixErr[i].fromErr != 0; ++i) {
            if(kWSAToPosixErr[i].fromErr == err) {
                Log("Mapping WSA error %d to POSIX %d", err, kWSAToPosixErr[i].toErr);
                return kWSAToPosixErr[i].toErr;
            }
        }
        Warn("No mapping for WSA error %d", err);
#endif
        return err;
    }

    int TCPSocket::mbedToNetworkErrCode(int err) {
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



    void TCPSocket::checkStreamError() {
        int err = _socket->last_error();
        Assert(err != 0);
        if (err > 0) {
            string errMsg = _socket->last_error_str();
            errMsg.erase(errMsg.find_last_not_of(" \n\r")+1);
            C4LogToAt(kC4WebSocketLog, kC4LogWarning,
                    "TCPSocket got POSIX error %d \"%s\"", err, errMsg.c_str());
            setError(POSIXDomain, socketToPosixErrCode(err), errMsg);
        } else {
            // Negative errors are assumed to be from mbedTLS.
            char msgbuf[100];
            mbedtls_strerror(err, msgbuf, sizeof(msgbuf));
            C4LogToAt(kC4WebSocketLog, kC4LogWarning,
                    "TCPSocket got mbedTLS error -0x%04X \"%s\"", -err, msgbuf);
            setError(NetworkDomain, mbedToNetworkErrCode(err), slice(msgbuf));
        }
    }


#pragma mark - CLIENT SOCKET:


    ClientSocket::ClientSocket(tls_context *tls)
    :TCPSocket(tls)
    { }


    bool ClientSocket::connect(const Address &addr) {
        // sockpp constructors can throw exceptions.
        try {
            string hostname(slice(addr.hostname));
            _socket = make_unique<tcp_connector>(inet_address{hostname, addr.port});
            if (addr.isSecure() && *_socket)
                return wrapTLS(addr.hostname);
            else
                return checkSocketFailure();

        } catch (const sockpp::sys_error &sx) {
            setError(POSIXDomain, sx.error(), slice(sx.what()));
            return false;
        } catch (const sockpp::getaddrinfo_error &gx) {
            C4ErrorDomain domain = NetworkDomain;
            int code;
            string messagebuf;
            if (gx.error() == EAI_NONAME || gx.error() == HOST_NOT_FOUND) {
                code = kC4NetErrUnknownHost;
                messagebuf = "Unknown hostname \"" + gx.hostname() + "\"";
            } else {
                code = kC4NetErrDNSFailure;
                messagebuf = "Error resolving hostname \"" + gx.hostname() + "\": " + gx.what();
            }
            setError(domain, code, slice(messagebuf));
            return false;
        }
    }


#pragma mark - RESPONDER SOCKET:


    ResponderSocket::ResponderSocket(tls_context *tls)
    :TCPSocket(tls)
    { }


    bool ResponderSocket::acceptSocket(stream_socket &&s) {
        return acceptSocket( make_unique<tcp_socket>(move(s)));
    }

    
    bool ResponderSocket::acceptSocket(unique_ptr<stream_socket> socket) {
        _socket = move(socket);
        return checkSocketFailure();
    }

} }
