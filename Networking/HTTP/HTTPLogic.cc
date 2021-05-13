//
// HTTPLogic.cc
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

#include "HTTPLogic.hh"
#include "TCPSocket.hh"
#include "WebSocketInterface.hh"
#include "c4ReplicatorTypes.h"
#include "Base64.hh"
#include "Error.hh"
#include "SecureRandomize.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"
#include "slice_stream.hh"
#include <regex>
#include <sstream>

namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;
    using namespace websocket;


    static constexpr unsigned kMaxRedirects = 10;


    optional<ProxySpec> HTTPLogic::sDefaultProxy;


    HTTPLogic::HTTPLogic(const Address &address,
                         bool handleRedirects)
    :_address(address)
    ,_handleRedirects(handleRedirects)
    ,_isWebSocket(address.scheme == "ws"_sl || address.scheme == "wss"_sl)
    ,_proxy(sDefaultProxy)
    { }


    HTTPLogic::HTTPLogic(const Address &address,
                         const websocket::Headers &requestHeaders,
                         bool handleRedirects)
    :HTTPLogic(address, handleRedirects)
    {
        _requestHeaders = requestHeaders;
    }


    HTTPLogic::~HTTPLogic() =default;


    void HTTPLogic::setHeaders(const websocket::Headers &requestHeaders) {
        Assert(_requestHeaders.empty());
        _requestHeaders = requestHeaders;
    }


    void HTTPLogic::setProxy(optional<ProxySpec> p) {
        _proxy = move(p);
        if (_proxy)
            _proxyAddress = Address(*_proxy);
        else
            _proxyAddress.reset();
    }


    const Address& HTTPLogic::directAddress() {
        return _proxy ? *_proxyAddress : _address;
    }


    bool HTTPLogic::connectingToProxy() {
        return _proxy && _isWebSocket && _lastDisposition != kContinue;
    }


    static void addHeader(stringstream &rq, const char *key, slice value) {
        if (value)
            rq << key << ": " << string(value) << "\r\n";
    }


    string HTTPLogic::requestToSend() {
        if (_lastDisposition == kAuthenticate) {
            if (_httpStatus == HTTPStatus::ProxyAuthRequired)
                Assert(_proxy && _proxy->username);
            else
                Assert(_authHeader);
        }

        stringstream rq;
        if (connectingToProxy()) {
            // CONNECT proxy: https://tools.ietf.org/html/rfc7231#section-4.3.6
            rq << "CONNECT " << string(slice(_address.hostname)) << ":" << _address.port;
        } else {
            rq << MethodName(_method) << " ";
            if (_proxy && _proxy->type == ProxyType::HTTP)
                rq << string(_address.url());
            else
                rq << string(slice(_address.path));
        }
        rq << " HTTP/1.1\r\n"
              "Host: " << string(slice(_address.hostname)) << ':' << _address.port << "\r\n";
        addHeader(rq, "User-Agent", _userAgent);

        if (_proxy && _proxy->username)
            addHeader(rq, "Proxy-Authorization", basicAuth(_proxy->username, _proxy->password));

        if (!connectingToProxy()) {
            if (_authChallenged)                                    // don't send auth until challenged
                addHeader(rq, "Authorization", _authHeader);

            if (_cookieProvider)
                addHeader(rq, "Cookie", _cookieProvider->cookiesForRequest(_address));

            if (_contentLength >= 0)
                rq << "Content-Length: " << _contentLength << "\r\n";

            _requestHeaders.forEach([&](slice name, slice value) {
                rq << string(name) << ": " << string(value) << "\r\n";
            });

            if (_isWebSocket) {
                // WebSocket handshake headers:
                uint8_t nonceBuf[16];
                mutable_slice nonceBytes(nonceBuf, sizeof(nonceBuf));
                SecureRandomize(nonceBytes);
                _webSocketNonce = base64::encode(nonceBytes);
                rq << "Connection: Upgrade\r\n"
                      "Upgrade: websocket\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "Sec-WebSocket-Key: " << _webSocketNonce << "\r\n";
                addHeader(rq, "Sec-WebSocket-Protocol", _webSocketProtocol);
            }
        }

        rq << "\r\n";
        return rq.str();
    }


    alloc_slice HTTPLogic::basicAuth(slice username, slice password) {
        string credential = base64::encode(string(username) + ':' + string(password));
        return alloc_slice("Basic " + credential);
    }


#pragma mark - RESPONSE HANDLING:


    HTTPLogic::Disposition HTTPLogic::receivedResponse(slice responseData) {
        _httpStatus = HTTPStatus::undefined;
        _statusMessage = nullslice;
        _responseHeaders.clear();
        _error = {};
        _authChallenge.reset();

        slice_istream in(responseData);
        if (parseStatusLine(in) && parseHeaders(in, _responseHeaders))
            _lastDisposition = handleResponse();
        else
            _lastDisposition = failure(WebSocketDomain, 400, "Received invalid HTTP"_sl);
        return _lastDisposition;
    }


    HTTPLogic::Disposition HTTPLogic::handleResponse() {
        if (_cookieProvider && !connectingToProxy()) {
            _responseHeaders.forEach("Set-Cookie"_sl, [&](slice header) {
                _cookieProvider->setCookie(_address, header);
            });
        }

        switch (_httpStatus) {
            case HTTPStatus::MovedPermanently:
            case HTTPStatus::Found:
            case HTTPStatus::TemporaryRedirect:
            case HTTPStatus::UseProxy:
                return handleRedirect();
            case HTTPStatus::Unauthorized:
                if (_authChallenged)
                    _authHeader = nullslice;
                else
                    _authChallenged = true;
                return handleAuthChallenge("Www-Authenticate"_sl, false);
            case HTTPStatus::ProxyAuthRequired:
                if (_proxy)
                    _proxy->username = _proxy->password = nullslice;
                return handleAuthChallenge("Proxy-Authenticate"_sl, true);
            case HTTPStatus::Upgraded:
                return handleUpgrade();
            default:
                if (!IsSuccess(_httpStatus))
                    return failure();
                else if (connectingToProxy())
                    return kContinue;
                else if (_isWebSocket)
                    return failure(WebSocketDomain, kCodeProtocolError,
                                   "Server failed to upgrade connection"_sl);
                else
                    return kSuccess;
        }
    }


    bool HTTPLogic::parseStatusLine(slice_istream &responseData) {
        slice version = responseData.readToDelimiter(" "_sl);
        uint64_t status = responseData.readDecimal();
        if (!version.hasPrefix("HTTP/"_sl) || status == 0 || status > INT_MAX)
            return false;
        _httpStatus = HTTPStatus(status);
        if (responseData.size == 0 || (responseData[0] != ' ' && responseData[0] != '\r'))
            return false;
        while (responseData.hasPrefix(' '))
            responseData.skip(1);
        slice message = responseData.readToDelimiter("\r\n"_sl);
        if (!message)
            return false;
        _statusMessage = alloc_slice(message);
        return true;
    }


    // Reads HTTP headers out of `responseData`. Assumes data ends with CRLFCRLF.
    bool HTTPLogic::parseHeaders(slice_istream &responseData, Headers &headers) {
        while (true) {
            slice line = responseData.readToDelimiter("\r\n"_sl);
            if (!line)
                return false;
            if (line.size == 0)
                break;  // empty line
            const uint8_t *colon = line.findByte(':');
            if (!colon)
                return false;
            slice name(line.buf, colon);
            line.setStart(colon+1);
            const uint8_t *nonSpace = line.findByteNotIn(" "_sl);
            if (!nonSpace)
                return false;
            slice value(nonSpace, line.end());
            headers.add(name, value);
        }
        return true;
    }


    HTTPLogic::Disposition HTTPLogic::handleRedirect() {
        if (!_handleRedirects)
            return failure();
        if (++_redirectCount > kMaxRedirects)
            return failure(NetworkDomain, kC4NetErrTooManyRedirects);

        C4Address newAddr;
        slice location = _responseHeaders["Location"_sl];
        if (location.hasPrefix('/')) {
            newAddr = _address;
            newAddr.path = location;
        } else {
            if (!C4Address::fromURL(location, &newAddr, nullptr)
                    || (newAddr.scheme != "http"_sl && newAddr.scheme != "https"_sl))
                return failure(NetworkDomain, kC4NetErrInvalidRedirect);
        }

        if (_httpStatus == HTTPStatus::UseProxy) {
            if (_proxy)
                return failure();
            _proxy = ProxySpec(ProxyType::HTTP, newAddr);
        } else {
            if (newAddr.hostname != _address.hostname)
                _authHeader = nullslice;
            _address = Address(newAddr);
        }
        return kRetry;
    }


    HTTPLogic::Disposition HTTPLogic::handleAuthChallenge(slice headerName, bool forProxy) {
        if (forProxy)
            Assert(_proxy);
        string authHeader(_responseHeaders[headerName]);
        // Parse the Authenticate header:
        regex authEx(R"((\w+)\s+(\w+)=((\w+)|"([^"]+)))");     // e.g. Basic realm="Foobar"
        smatch m;
        if (!regex_search(authHeader, m, authEx))
            return failure();
        AuthChallenge challenge((forProxy ? *_proxyAddress : _address), forProxy);
        challenge.type = m[1].str();
        challenge.key = m[2].str();
        challenge.value = m[4].str();
        if (challenge.value.empty())
            challenge.value = m[5].str();
        _authChallenge = challenge;
        if (!forProxy)
            _authChallenged = true;
        return kAuthenticate;
    }


    HTTPLogic::Disposition HTTPLogic::handleUpgrade() {
        if (!_isWebSocket)
            return failure(WebSocketDomain, kCodeProtocolError);

        if (_responseHeaders["Connection"_sl].caseEquivalentCompare(
                "upgrade"_sl) != 0 ||
            _responseHeaders["Upgrade"_sl] != "websocket"_sl) {
          return failure(WebSocketDomain, kCodeProtocolError,
                         "Server failed to upgrade connection"_sl);
        }

        if (_webSocketProtocol && _responseHeaders["Sec-Websocket-Protocol"_sl] != _webSocketProtocol) {
            return failure(WebSocketDomain, 403, "Server did not accept protocol"_sl);
        }

        // Check the returned nonce:
        if (_responseHeaders["Sec-Websocket-Accept"_sl] != slice(webSocketKeyResponse(_webSocketNonce)))
            return failure(WebSocketDomain, kCodeProtocolError,
                           "Server returned invalid nonce"_sl);

        return kSuccess;
    }


    string HTTPLogic::webSocketKeyResponse(const string &nonce) {
        SHA1 digest{slice(nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")};
        return digest.asBase64();
    }


    HTTPLogic::Disposition HTTPLogic::failure(C4ErrorDomain domain, int code, slice message) {
        Assert(code != 0);
        _error = c4error_make(domain, code, message);
        return kFailure;
    }


    HTTPLogic::Disposition HTTPLogic::failure(ClientSocket &socket) {
        _error = socket.error();
        Assert(_error.code != 0);
        return kFailure;
    }


    HTTPLogic::Disposition HTTPLogic::failure() {
        return failure(WebSocketDomain, int(_httpStatus), _statusMessage);
    }


    HTTPLogic::Disposition HTTPLogic::sendNextRequest(ClientSocket &socket, slice body) {
        bool connected;
        if (_lastDisposition == kContinue) {
            Assert(socket.connected());
            connected = !_address.isSecure() || socket.wrapTLS(_address.hostname);
        } else {
            Assert(!socket.connected());
            connected = socket.connect(directAddress());
        }
        if (!connected)
            return failure(socket);

        C4LogToAt(kC4WebSocketLog, kC4LogVerbose, "Sending request to %s:\n%s",
                  (_lastDisposition == kContinue ? "proxy tunnel"
                                                 : string(directAddress().url()).c_str()),
                  formatHTTP(slice(requestToSend())).c_str());
        if (socket.write_n(requestToSend()) < 0 || socket.write_n(body) < 0)
            return failure(socket);
        alloc_slice response = socket.readToDelimiter("\r\n\r\n"_sl);
        if (!response)
            return failure(socket);
        C4LogToAt(kC4WebSocketLog, kC4LogVerbose, "Got response:\n%s", formatHTTP(response).c_str());

        Disposition disposition = receivedResponse(response);
        if (disposition == kFailure && _error.domain == WebSocketDomain
                                    && _error.code == int(_httpStatus)) {
            // Look for a more detailed HTTP error message in the response body:
            if (_responseHeaders["Content-Type"_sl].hasPrefix("application/json"_sl)) {
                alloc_slice responseBody;
                if (socket.readHTTPBody(_responseHeaders, responseBody)) {
                    Doc json = Doc::fromJSON(responseBody);
                    if (slice reason = json["reason"].asString(); reason)
                        _error = c4error_make(WebSocketDomain, int(_httpStatus), reason);
                }
            }
        }
        return disposition;
    }


    string HTTPLogic::formatHTTP(slice http) {
        slice_istream in(http);
        stringstream s;
        bool first = true;
        while (true) {
            slice line = in.readToDelimiter("\r\n"_sl);
            if (line.size == 0)
                break;
            if (!first)
                s << '\n';
            first = false;
            s << '\t';
            s.write((const char*)line.buf, line.size);
        }
        return s.str();
    }


} }
