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


    XSocket::XSocket()
    { }


    XSocket::~XSocket()
    { }


    void XSocket::setTLSContext(tls_context *tls) {
        _tlsContext = tls;
    }

    tls_context* XSocket::TLSContext() {
        return _tlsContext;
    }

    void XSocket::checkSocketFailure() {
        if (*_socket)
            return;

        // TLS handshake failed:
        if (_socket->last_error() == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            // Some more specific errors for certificate validation failures:
            auto tlsSocket = (tls_socket*)_socket.get();
            uint32_t flags = tlsSocket->peer_certificate_status();
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
        ssize_t written = _socket->write(data.buf, data.size);
        if (written < 0) {
            if (_socket->last_error() == EBADF)
                return 0;
            _throwLastError();
        }
        return size_t(written);
    }


    size_t XSocket::write_n(slice data) {
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


    slice XSocket::readToDelimiter(slice delim) {
        if (_inputLen > 0 && _inputStart > _input.buf) {
            // Slide unread input down to start of buffer:
            memmove((void*)_input.buf, _inputStart, _inputLen);
            _inputStart = (uint8_t*)_input.buf;
        }

        while (true) {
            // Look for delimiter:
            slice found = slice(_inputStart, _inputLen).find(delim);
            if (found) {
                slice result(_inputStart, found.buf);
                _inputLen -= result.size + found.size;
                _inputStart = (uint8_t*)found.end();
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


#pragma mark - UTILITIES:


    void XSocket::_throwLastError() {
        int err = _socket->last_error();
        Assert(err != 0);
        if (err > 0) {
            error::_throw(error::POSIX, err);
        } else {
            // Negative errors are assumed to be from mbedTLS.
            char msgbuf[100];
            mbedtls_strerror(err, msgbuf, sizeof(msgbuf));
            Warn("Got mbedTLS error -0x%04X \"%s\"; throwing exception...", -err, msgbuf);
            static constexpr struct {int mbed; int net;} kMbedToNetErr[] = {
                {MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, kNetErrTLSCertUntrusted},
                {MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE, kNetErrTLSHandshakeFailed},
                {0, kNetErrUnknown}
            };
            int netErr = kNetErrUnknown;
            for (int i = 0; kMbedToNetErr[i].mbed != 0; ++i) {
                if (kMbedToNetErr[i].mbed == err) {
                    netErr = kMbedToNetErr[i].net;
                    break;
                }
            }
            error(error::Network, netErr, msgbuf)._throw();
        }
    }


    void XSocket::_throwBadHTTP() {
        error{error::WebSocket, 599, "Invalid HTTP syntax"}._throw(); // TODO: error code
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


    static void normalizeHeaderCase(string &str) {
        bool caps = true;
        for (auto &c : str) {
            if (isalpha(c)) {
                c = (char)(caps ? toupper(c) : tolower(c));
                caps = false;
            } else {
                caps = true;
            }
        }
    }


    AllocedDict XSocket::readHeaders() {
        Assert(_readState == kHeaders);
        Encoder enc;
        enc.beginDict();
        while (true) {
            slice line = readToDelimiter("\r\n"_sl);
            if (!line)
                _throwBadHTTP();
            if (line.size == 0)
                break;  // empty line
            const uint8_t *colon = line.findByte(':');
            if (!colon)
                _throwBadHTTP();
            string name(slice(line.buf, colon));
            normalizeHeaderCase(name);
            line.setStart(colon+1);
            const uint8_t *nonSpace = line.findByteNotIn(" "_sl);
            if (!nonSpace)
                _throwBadHTTP();
            slice value(nonSpace, line.end());
            enc.writeKey(name);
            enc.writeString(value);
        }
        _readState = kBody;
        enc.endDict();
        return AllocedDict(enc.finishDoc().allocedData());
    }


    alloc_slice XSocket::readHTTPBody(AllocedDict headers) {
        Assert(_readState == kBody);
        alloc_slice body;
        int64_t contentLength;
        if (getIntHeader(headers, "Content-Length"_sl, contentLength)) {
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


    void XSocket::writeHeaders(stringstream &rq, Dict headers) {
        Assert(_writeState == kHeaders);
        for (Dict::iterator i(headers); i; ++i)
            rq << string(i.keyString()) << ": " << string(i.value().toString()) << "\r\n";
    }


    bool XSocket::getIntHeader(Dict headers, slice key, int64_t &value) {
        slice v = headers[key].asString();
        if (!v)
            return false;
        try {
            value = stoi(string(v));
            return true;
        } catch (exception &x) {
            return false;
        }
    }


#pragma mark - HTTP CLIENT:


    HTTPClientSocket::HTTPClientSocket(repl::Address addr)
    :_addr(addr)
    { }


    void HTTPClientSocket::connect() {
        string hostname(slice(_addr.hostname));
        auto socket = make_unique<tcp_connector>(inet_address{hostname, _addr.port});
        if (_addr.isSecure() && *socket) {
            if (!_tlsContext)
                _tlsContext = _tlsContext = &tls_context::default_context();
            _socket = TLSContext()->wrap_socket(move(socket), tls_context::CLIENT, hostname);
        } else {
            _socket = move(socket);
        }
        checkSocketFailure();
    }


    void HTTPClientSocket::sendHTTPRequest(const string &method,
                                           function_ref<void(stringstream&)> fn)
    {
        Assert(_writeState == kRequestLine);
        stringstream rq;
        rq << method << " " << string(slice(_addr.path)) << " HTTP/1.1\r\n"
            "Host: " << string(slice(_addr.hostname)) << "\r\n";
        _writeState = kHeaders;
        fn(rq);
        rq << "\r\n";
        _writeState = kBody;
        write_n(slice(rq.str()));
    }


    void HTTPClientSocket::sendHTTPRequest(const string &method, Dict headers, slice body) {
        sendHTTPRequest(method, [&](stringstream &rq) {
            writeHeaders(rq, headers);
            if (!headers["Content-Length"])
                rq << "Content-Length: " << body.size << "\r\n";
        });
        write_n(body);
        _writeState = kEnd;
    }


    HTTPClientSocket::HTTPResponse HTTPClientSocket::readHTTPResponse() {
        Assert(_readState == kStatusLine);
        HTTPResponse response = {};

        string responseData = string(readToDelimiter("\r\n"_sl));
        if (responseData.empty())
            _throwBadHTTP();
        regex responseParser(R"(^HTTP/(\d\.\d) (\d+) (.*))");
        smatch m;
        if (!regex_search(responseData, m, responseParser))
            error::_throw(error::Network, kC4NetErrUnknown);
        response.status = REST::HTTPStatus(stoi(m[2].str()));
        response.message = m[3].str();

        _readState = kHeaders;
        response.headers = readHeaders();
        _readState = kBody;
        return response;
    }


    string HTTPClientSocket::sendWebSocketRequest(Dict headers, const string &protocol) {
        Assert(_writeState == kStatusLine);
        uint8_t nonceBuf[16];
        slice nonceBytes(nonceBuf, sizeof(nonceBuf));
        SecureRandomize(nonceBytes);
        string nonce = nonceBytes.base64String();

        sendHTTPRequest("GET", [&](stringstream &rq) {
            rq << "Connection: Upgrade\r\n"
                  "Upgrade: websocket\r\n"
                  "Sec-WebSocket-Version: 13\r\n"
                  "Sec-WebSocket-Key: " << nonce << "\r\n";
            if (!protocol.empty())
                rq << "Sec-WebSocket-Protocol: " << protocol << "\r\n";
            writeHeaders(rq, headers);
        });
        return nonce;
    }


    bool HTTPClientSocket::checkWebSocketResponse(const HTTPResponse &rs,
                                         const string &nonce,
                                         const string &requiredProtocol,
                                         CloseStatus &status) {
        if (rs.status != REST::HTTPStatus::Upgraded) {
            if (IsSuccess(rs.status))
                status = {kWebSocketClose, kCodeProtocolError, "Unexpected HTTP response status"_sl};
            else
                status = {kWebSocketClose, int(rs.status), alloc_slice(rs.message)};
            return false;
        }
        if (rs.headers["Connection"_sl].asString() != "Upgrade"_sl
                || rs.headers["Upgrade"_sl].asString() != "websocket"_sl) {
            status = {kWebSocketClose, kCodeProtocolError, "Server failed to upgrade connection"_sl};
            return false;
        }
        if (!requiredProtocol.empty()
                && rs.headers["Sec-Websocket-Protocol"_sl].asString() != slice(requiredProtocol)) {
            status = {kWebSocketClose, 403, "Server did not accept BLIP replication protocol"_sl};
            return false;
        }

        // Check the returned nonce:
        SHA1 digest{slice(nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")};
        string resultNonce = slice(&digest, sizeof(digest)).base64String();
        if (rs.headers["Sec-Websocket-Accept"].asString() != slice(resultNonce)) {
            status = {kWebSocketClose, kCodeProtocolError, "Server returned invalid nonce"_sl};
            return false;
        }
        return true;
    }


#pragma mark - HTTP SERVER:


    void HTTPResponderSocket::acceptSocket(stream_socket &&s, bool useTLS)
    {
        acceptSocket( make_unique<tcp_socket>(move(s)), useTLS );
    }

    void HTTPResponderSocket::acceptSocket(unique_ptr<stream_socket> socket, bool useTLS) {
        if (_tlsContext)
            _socket = _tlsContext->wrap_socket(move(socket), tls_context::SERVER, "");
        else
            _socket = move(socket);
        checkSocketFailure();
    }


    HTTPResponderSocket::HTTPRequest HTTPResponderSocket::readHTTPRequest() {
        Assert(_readState == kRequestLine);
        auto method = REST::MethodNamed(readToDelimiter(" "_sl));

        slice uri = readToDelimiter(" "_sl);
        auto q = uri.findByteOrEnd('?');
        string path(slice(uri.buf,q));
        string query;
        if (q != uri.end())
            query = string(slice(q + 1, uri.end()));

        slice version = readToDelimiter("\r\n"_sl);
        if (!version.hasPrefix("HTTP/"_sl))
            _throwBadHTTP();
        _readState = kHeaders;
        return {method, path, query, readHeaders()};
    }


    void HTTPResponderSocket::writeResponseLine(REST::HTTPStatus status, slice message) {
        Assert(_writeState == kRequestLine);
        if (!message) {
            const char *defaultMessage = StatusMessage(status);
            if (defaultMessage)
                message = slice(defaultMessage);
        }
        write_n(format("HTTP/1.0 %d %.*s\r\n", status, SPLAT(message)));
        _writeState = kHeaders;
    }


    void HTTPResponderSocket::writeHeader(slice name, slice value) {
        Assert(_writeState == kHeaders);
        write_n(format("%.*s: %.*s\r\n", SPLAT(name), SPLAT(value)));
    }


    void HTTPResponderSocket::endHeaders() {
        Assert(_writeState == kHeaders);
        write_n("\r\n"_sl);
        _writeState = kBody;
    }

} }
