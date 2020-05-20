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
#include "TLSContext.hh"
#include "Poller.hh"
#include "Headers.hh"
#include "HTTPLogic.hh"
#include "Certificate.hh"
#include "NetworkInterfaces.hh"
#include "WebSocketInterface.hh"
#include "SecureRandomize.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "sockpp/exception.h"
#include "sockpp/inet6_address.h"
#include "sockpp/tcp_acceptor.h"
#include "sockpp/connector.h"
#include "sockpp/mbedtls_context.h"
#include "sockpp/tls_socket.h"
#include "PlatformIO.hh"
#include <chrono>
#include <regex>
#include <string>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdocumentation"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#pragma clang diagnostic pop

namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;
    using namespace sockpp;
    using namespace litecore::net;
    using namespace litecore::websocket;

    static constexpr size_t kInitialDelimitedReadBufferSize = 1024;


    static chrono::microseconds secsToMicrosecs(double secs) {
        return chrono::microseconds(long(secs * 1e6));
    }


    void TCPSocket::initialize() {
        static once_flag f;
        call_once(f, [=] {
            socket::initialize();
        });
    }


    #define WSLog (*(LogDomain*)kC4WebSocketLog)
    #define LOG(LEVEL, ...) LogToAt(WSLog, LEVEL, ##__VA_ARGS__)


    TCPSocket::TCPSocket(bool isClient, TLSContext *tls)
    :_tlsContext(tls)
    ,_isClient(isClient) {
        initialize();
    }


    TCPSocket::~TCPSocket() {
        _socket.reset(); // Make sure socket closes before _tlsContext does
        if (_onClose)
            _onClose();
    }


    bool TCPSocket::setSocket(unique_ptr<stream_socket> socket) {
        Assert(!_socket);
        _socket = move(socket);
        if (!checkSocketFailure())
            return false;
        _setTimeout(_timeout);
        return true;
    }

    TLSContext* TCPSocket::tlsContext() {
        return _tlsContext;
    }


    bool TCPSocket::wrapTLS(slice hostname) {
        if (!_tlsContext)
            _tlsContext = new TLSContext(_isClient ? TLSContext::Client : TLSContext::Server);
        string hostnameStr(hostname);
        auto oldSocket = move(_socket);
        return setSocket(_tlsContext->_context->wrap_socket(move(oldSocket),
                                            (_isClient ? tls_context::CLIENT : tls_context::SERVER),
                                            hostnameStr.c_str()));
    }


    bool TCPSocket::connected() const {
        return _socket && !_socket->is_shutdown();
    }


    void TCPSocket::close() {
        if (_socket)
            _socket->shutdown();
    }


    sockpp::stream_socket* TCPSocket::actualSocket() const {
        if (auto socket = _socket.get(); !socket->is_open())
            return nullptr;
        else if (auto tlsSock = dynamic_cast<tls_socket*>(socket); tlsSock)
            return &tlsSock->stream();
        else
            return socket;
    }


    string TCPSocket::peerAddress() {
        if (auto socket = actualSocket(); socket) {
            switch (auto addr = socket->peer_address(); addr.family()) {
                case AF_INET:   return inet_address(addr).to_string();
                case AF_INET6:  return inet6_address(addr).to_string();
            }
        }
        return "";
    }


    Retained<crypto::Cert> TCPSocket::peerTLSCertificate() {
        auto tlsSock = dynamic_cast<tls_socket*>(_socket.get());
        if (!tlsSock)
            return nullptr;
        string certData = tlsSock->peer_certificate();
        if (certData.empty())
            return nullptr;
        return new crypto::Cert(slice(certData));
    }


#pragma mark - CLIENT SOCKET:


    ClientSocket::ClientSocket(TLSContext *tls)
    :TCPSocket(true, tls)
    { }


    bool ClientSocket::connect(const Address &addr) {
        // sockpp constructors can throw exceptions.
        try {
            string hostname(slice(addr.hostname));

            unique_ptr<sock_address> sockAddr;
            optional<IPAddress> ipAddr = IPAddress::parse(hostname);
            if (ipAddr) {
                // hostname is numeric, either IPv4 or IPv6:
                sockAddr = ipAddr->sockppAddress(addr.port);
            } else {
                // Otherwise it's a DNS name; let sockpp resolve it:
                sockAddr = make_unique<inet_address>(hostname, addr.port);
            }

            auto socket = make_unique<connector>();
            socket->connect(*sockAddr, secsToMicrosecs(timeout()));
            return setSocket(move(socket))
                && (!addr.isSecure() || wrapTLS(addr.hostname));

        } catch (const sockpp::sys_error &sx) {
            auto e = error::convertException(sx);
            setError(C4ErrorDomain(e.domain), e.code, slice(e.what()));
            return false;
        } catch (const sockpp::getaddrinfo_error &gx) {
            auto e = error::convertException(gx);
            setError(C4ErrorDomain(e.domain), e.code, slice(e.what()));
            return false;
        }
    }


#pragma mark - RESPONDER SOCKET:


    ResponderSocket::ResponderSocket(TLSContext *tls)
    :TCPSocket(false, tls)
    { }


    bool ResponderSocket::acceptSocket(stream_socket &&s) {
        return setSocket( make_unique<tcp_socket>(move(s)));
    }


    bool ResponderSocket::acceptSocket(unique_ptr<stream_socket> socket) {
        return setSocket(move(socket));
    }


#pragma mark - READ/WRITE:


#ifdef _WIN32
    static constexpr int kErrWouldBlock = WSAEWOULDBLOCK;
#else
    static constexpr int kErrWouldBlock = EWOULDBLOCK;
#endif


    ssize_t TCPSocket::write(slice data) {
        if (data.size == 0)
            return 0;
        ssize_t written = _socket->write(data.buf, data.size);
        if (written < 0) {
            if (_nonBlocking && _socket->last_error() == kErrWouldBlock)
                return 0;
            checkStreamError();
        } else if (written == 0) {
            _eofOnWrite = true;
        }
        return written;
    }


    ssize_t TCPSocket::write_n(slice data) {
        if (data.size == 0)
            return 0;
        ssize_t written = _socket->write_n(data.buf, data.size);
        if (written < 0) {
            if (_nonBlocking && _socket->last_error() == kErrWouldBlock)
                return 0;
            checkStreamError();
        }
        return written;
    }


    ssize_t TCPSocket::write(vector<slice> &ioByteRanges) {
        // We are going to cast slice[] to iovec[] since they are identical structs,
        // but make sure they are actualy identical:
        static_assert(sizeof(iovec) == sizeof(slice)
                      && sizeof(iovec::iov_base) == sizeof(slice::buf)
                      && sizeof(iovec::iov_len) == sizeof(slice::size),
                      "iovec and slice are incompatible");
        ssize_t written = _socket->write(reinterpret_cast<vector<iovec>&>(ioByteRanges));
        if (written < 0) {
            if (_socket->last_error() == kErrWouldBlock)
                return 0;
            checkStreamError();
            return written;
        }

        ssize_t remaining = written;
        for (auto i = ioByteRanges.begin(); i != ioByteRanges.end(); ++i) {
            remaining -= i->size;
            if (remaining < 0) {
                // This slice was only partly written (or unwritten). Adjust its start:
                i->moveStart(i->size + remaining);
                // Remove all prior slices:
                ioByteRanges.erase(ioByteRanges.begin(), i);
                return written;
            }
        }
        // Looks like everything was written:
        ioByteRanges.clear();
        return written;
    }


    // Primitive unbuffered read call. Returns 0 on EOF, -1 on error (and sets _error).
    // Assumes EWOULDBLOCK is not an error, since it happens normally in non-blocking reads.
    ssize_t TCPSocket::_read(void *dst, size_t byteCount) {
        Assert(byteCount > 0);
        ssize_t n = _socket->read(dst, byteCount);
        if (n < 0) {
            if (_socket->last_error() == kErrWouldBlock)
                return 0;
            checkStreamError();
        } else if (n == 0) {
            _eofOnRead = true;
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
                body.resize(size_t(contentLength));
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
                ssize_t n = read((void*)&body[length], body.size - length);
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


#pragma mark - NONBLOCKING / SELECT:


    bool TCPSocket::setTimeout(double secs) {
        if (secs == _timeout)
            return true;
        if (_socket && !_setTimeout(secs))
            return false;
        _timeout = secs;
        return true;
    }


    bool TCPSocket::_setTimeout(double secs) {
        std::chrono::microseconds us = secsToMicrosecs(secs);
        return _socket->read_timeout(us) && _socket->write_timeout(us);
    }


    bool TCPSocket::setNonBlocking(bool nb) {
        bool ok = _socket->set_non_blocking(nb);
        if (ok)
            _nonBlocking = nb;
        else
            checkStreamError();
        return ok;
    }


    int TCPSocket::fileDescriptor() {
        if (auto socket = actualSocket(); socket)
            return socket->handle();
        else
            return -1;
    }


    void TCPSocket::onReadable(function<void()> listener) {
        Poller::instance().addListener(fileDescriptor(), Poller::kReadable, listener);
    }


    void TCPSocket::onWriteable(function<void()> listener) {
        Poller::instance().addListener(fileDescriptor(), Poller::kWriteable, listener);
    }


    void TCPSocket::interrupt() {
        if(fileDescriptor() >= 0) {
            // If an interrupt is called with an invalid socket, the poller's
            // loop will exit, so don't do that
            Poller::instance().interrupt(fileDescriptor());
        }
    }


#pragma mark - ERRORS:


    void TCPSocket::setError(C4ErrorDomain domain, int code, slice message) {
        Assert(code != 0);
        _error = c4error_make(domain, code, message);
    }


    bool TCPSocket::checkSocketFailure() {
        if (*_socket)
            return true;

        // TLS handshake failed:
        int err = _socket->last_error();
        if (err == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            // Some more specific errors for certificate validation failures, based on flags:
            auto tlsSocket = (tls_socket*)_socket.get();
            uint32_t flags = tlsSocket->peer_certificate_status();
            LOG(Error, "TCPSocket TLS handshake failed; cert verify status 0x%02x", flags);
            if (flags != 0 && flags != UINT32_MAX) {
                string message = tlsSocket->peer_certificate_status_message();
                int code;
                if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
                    code = kNetErrTLSCertUnknownRoot;
                else if (flags & MBEDTLS_X509_BADCERT_REVOKED)
                    code = kNetErrTLSCertRevoked;
                else if (flags & MBEDTLS_X509_BADCERT_EXPIRED)
                    code = kNetErrTLSCertExpired;
                else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
                    code = kNetErrTLSCertNameMismatch;
                else if (flags & MBEDTLS_X509_BADCERT_OTHER)
                    code = kNetErrTLSCertUntrusted;
                else
                    code = kNetErrTLSHandshakeFailed;
                setError(NetworkDomain, code, slice(message));
            }
        } else if (err <= mbedtls_context::FATAL_ERROR_ALERT_BASE
                                && err >= mbedtls_context::FATAL_ERROR_ALERT_BASE - 0xFF) {
            // Handle TLS 'fatal alert' when peer rejects our cert:
            auto alert = mbedtls_context::FATAL_ERROR_ALERT_BASE - err;
            LOG(Error, "TCPSocket TLS handshake failed with fatal alert %d", alert);
            int code;
            if (alert == MBEDTLS_SSL_ALERT_MSG_NO_CERT) {
                code = kNetErrTLSCertRequiredByPeer;
            } else if (alert >= MBEDTLS_SSL_ALERT_MSG_BAD_CERT
                            && alert <= MBEDTLS_SSL_ALERT_MSG_ACCESS_DENIED) {
                code = kNetErrTLSCertRejectedByPeer;
            } else {
                code = kNetErrTLSHandshakeFailed;
            }
            setError(NetworkDomain, code, nullslice);
        } else {
            checkStreamError();
        }
        return false;
    }


    static int socketToPosixErrCode(int err) {
#ifdef WIN32
        static constexpr struct {long fromErr; int toErr;} kWSAToPosixErr[] = {
            {WSA_INVALID_HANDLE, EBADF},
            {WSA_NOT_ENOUGH_MEMORY, ENOMEM},
            {WSA_INVALID_PARAMETER, EINVAL},
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
            {0, 0}
        };

        for(int i = 0; kWSAToPosixErr[i].fromErr != 0; ++i) {
            if(kWSAToPosixErr[i].fromErr == err) {
                //Log("Mapping WSA error %d to POSIX %d", err, kWSAToPosixErr[i].toErr);
                return kWSAToPosixErr[i].toErr;
            }
        }
#endif
        return err;
    }


    static int mbedToNetworkErrCode(int err) {
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
            err = socketToPosixErrCode(err);
            string errStr = error::_what(error::POSIX, err);
            LOG(Warning, "%s got POSIX error %d \"%s\"",
                (_isClient ? "ClientSocket" : "ResponderSocket"),
                err, errStr.c_str());
            if (err == EWOULDBLOCK)     // Occurs in blocking mode when I/O times out
                setError(NetworkDomain, kC4NetErrTimeout);
            else
                setError(POSIXDomain, err);
        } else {
            // Negative errors are assumed to be from mbedTLS.
            char msgbuf[100];
            mbedtls_strerror(err, msgbuf, sizeof(msgbuf));
            LOG(Warning, "%s got mbedTLS error -0x%04X \"%s\"",
                (_isClient ? "ClientSocket" : "ResponderSocket"),
                -err, msgbuf);
            setError(NetworkDomain, mbedToNetworkErrCode(err), slice(msgbuf));
        }
    }
} }
