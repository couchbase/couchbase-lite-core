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
#include "XSocket.hh"
#include "WebSocketInterface.hh"
#include "SecureRandomize.hh"
#include "SecureDigest.hh"
#include <regex>
#include <sstream>

namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;
    using namespace repl;
    using namespace websocket;


    static constexpr unsigned kMaxRedirects = 10;


    HTTPLogic::HTTPLogic(const repl::Address &address,
                         const Headers &requestHeaders,
                         bool handleRedirects)
    :_address(address)
    ,_requestHeaders(requestHeaders)
    ,_handleRedirects(handleRedirects)
    ,_isWebSocket(address.scheme == "ws"_sl || address.scheme == "wss"_sl)
    { }


    HTTPLogic::~HTTPLogic()
    { }


    void HTTPLogic::setProxy(ProxyType type, repl::Address addr) {
        _proxyType = type;
        _proxyAddress.reset(new Address(addr));
    }


    const Address& HTTPLogic::directAddress() {
        if (_proxyType == kNoProxy)
            return _address;
        else
            return *_proxyAddress;
    }


    static void addHeader(stringstream &rq, const char *key, slice value) {
        if (value)
            rq << key << ": " << string(value) << "\r\n";
    }


    string HTTPLogic::requestToSend() {
        if (_lastDisposition == kAuthenticate) {
            if (_httpStatus == HTTPStatus::ProxyAuthRequired)
                Assert(_proxyAuthHeader);
            else
                Assert(_authHeader);
        }

        stringstream rq;
        rq << MethodName(_method) << " ";
        if (_proxyType == kHTTPProxy)
            rq << string(_address.url());
        else
            rq << string(slice(_address.path));
        rq << " HTTP/1.1\r\n"
              "Host: " << string(slice(_address.hostname)) << ':' << _address.port << "\r\n";
        addHeader(rq, "User-Agent", _userAgent);
        addHeader(rq, "Authorization", _authHeader);
        addHeader(rq, "Proxy-Authorization", _proxyAuthHeader);
        if (_contentLength >= 0)
            rq << "Content-Length: " << _contentLength << "\r\n";
        _requestHeaders.forEach([&](slice name, slice value) {
            rq << string(name) << ": " << string(value) << "\r\n";
        });

        if (_isWebSocket) {
            // WebSocket handshake headers:
            uint8_t nonceBuf[16];
            slice nonceBytes(nonceBuf, sizeof(nonceBuf));
            SecureRandomize(nonceBytes);
            _webSocketNonce = nonceBytes.base64String();
            rq << "Connection: Upgrade\r\n"
                  "Upgrade: websocket\r\n"
                  "Sec-WebSocket-Version: 13\r\n"
                  "Sec-WebSocket-Key: " << _webSocketNonce << "\r\n";
            addHeader(rq, "Sec-WebSocket-Protocol", _webSocketProtocol);
        }

        rq << "\r\n";
        return rq.str();
    }


    alloc_slice HTTPLogic::basicAuth(slice username, slice password) {
        string credential = slice(string(username) + ':' + string(password)).base64String();
        return alloc_slice("Basic " + credential);
    }


#pragma mark - RESPONSE HANDLING:


    HTTPLogic::Disposition HTTPLogic::receivedResponse(slice responseData) {
        _httpStatus = HTTPStatus::undefined;
        _statusMessage = nullslice;
        _responseHeaders.clear();
        _error.reset();
        _authChallenge.reset();

        if (parseStatusLine(responseData) && parseHeaders(responseData, _responseHeaders))
            _lastDisposition = handleResponse();
        else
            _lastDisposition = failure(error::WebSocket, 400, "Received invalid HTTP"_sl);
        return _lastDisposition;
    }


    HTTPLogic::Disposition HTTPLogic::handleResponse() {
        switch (_httpStatus) {
            case HTTPStatus::MovedPermanently:
            case HTTPStatus::Found:
            case HTTPStatus::TemporaryRedirect:
            case HTTPStatus::UseProxy:
                return handleRedirect();
            case HTTPStatus::Unauthorized:
                _authHeader = nullslice;
                return handleAuthChallenge("Www-Authenticate"_sl, false);
            case HTTPStatus::ProxyAuthRequired:
                _proxyAuthHeader = nullslice;
                return handleAuthChallenge("Proxy-Authenticate"_sl, true);
            case HTTPStatus::Upgraded:
                return handleUpgrade();
            default:
                if (!IsSuccess(_httpStatus))
                    return failure(error::WebSocket, int(_httpStatus));
                else if (_isWebSocket)
                    return failure(error::WebSocket, kCodeProtocolError,
                                   "Server failed to upgrade connection"_sl);
                else
                    return kSuccess;
        }
    }


    bool HTTPLogic::parseStatusLine(slice &responseData) {
        slice version = responseData.readToDelimiter(" "_sl);
        uint64_t status = responseData.readDecimal();
        if (!version.hasPrefix("HTTP/"_sl) || status == 0 || status > INT_MAX)
            return false;
        _httpStatus = HTTPStatus(status);
        if (responseData.size == 0 || (responseData[0] != ' ' && responseData[0] != '\r'))
            return false;
        while (responseData.hasPrefix(' '))
            responseData.moveStart(1);
        slice message = responseData.readToDelimiter("\r\n"_sl);
        if (!message)
            return false;
        _statusMessage = alloc_slice(message);
        return true;
    }


    bool HTTPLogic::parseHeaders(slice &responseData, Headers &headers) {
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
            return failure(error::WebSocket, int(_httpStatus));
        if (++_redirectCount > kMaxRedirects)
            return failure(error::Network, kC4NetErrTooManyRedirects);

        C4Address newAddr;
        if (!c4address_fromURL(_responseHeaders["Location"_sl], &newAddr, nullptr)
                || (newAddr.scheme != "http"_sl && newAddr.scheme != "https"_sl))
            return failure(error::Network, kC4NetErrInvalidRedirect);

        if (_httpStatus == HTTPStatus::UseProxy) {
            if (_proxyType != kNoProxy)
                return failure(error::Network, int(_httpStatus));
            _proxyType = kHTTPProxy;
            _proxyAddress.reset(new Address(newAddr));
        } else {
            if (newAddr.hostname != _address.hostname)
                _authHeader = nullslice;
            _address = Address(newAddr);
        }
        return kRetry;
    }


    HTTPLogic::Disposition HTTPLogic::handleAuthChallenge(slice headerName, bool forProxy) {
        string authHeader(_responseHeaders[headerName]);
        // Parse the Authenticate header:
        regex authEx(R"((\w+)\s+(\w+)=((\w+)|"([^"]+)))");     // e.g. Basic realm="Foobar"
        smatch m;
        if (!regex_search(authHeader, m, authEx))
            return failure(error::WebSocket, 400);
        AuthChallenge challenge(forProxy ? *_proxyAddress : _address, forProxy);
        challenge.type = m[1].str();
        challenge.key = m[2].str();
        challenge.value = m[4].str();
        if (challenge.value.empty())
            challenge.value = m[5].str();
        _authChallenge = challenge;
        return kAuthenticate;
    }


    HTTPLogic::Disposition HTTPLogic::handleUpgrade() {
        if (!_isWebSocket)
            return failure(error::WebSocket, kCodeProtocolError);

        if (_responseHeaders["Connection"_sl] != "Upgrade"_sl
                || _responseHeaders["Upgrade"_sl] != "websocket"_sl) {
            return failure(error::WebSocket, kCodeProtocolError,
                           "Server failed to upgrade connection"_sl);
        }

        if (_webSocketProtocol && _responseHeaders["Sec-Websocket-Protocol"_sl] != _webSocketProtocol) {
            return failure(error::WebSocket, 403, "Server did not accept protocol"_sl);
        }

        // Check the returned nonce:
        SHA1 digest{slice(_webSocketNonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")};
        string resultNonce = slice(&digest, sizeof(digest)).base64String();
        if (_responseHeaders["Sec-Websocket-Accept"_sl] != slice(resultNonce))
            return failure(error::WebSocket, kCodeProtocolError,
                           "Server returned invalid nonce"_sl);

        return kSuccess;
    }


    HTTPLogic::Disposition HTTPLogic::failure(error::Domain domain, int code, slice message) {
        _error = litecore::error(domain, code, string(message));
        return kFailure;
    }


    HTTPLogic::Disposition HTTPLogic::sendNextRequest(XClientSocket &socket, slice body) {
        socket.connect(directAddress());
        Debug("Sending request: %s", requestToSend().c_str());
        socket.write_n(requestToSend());
        socket.write_n(body);
        return receivedResponse(socket.readToDelimiter("\r\n\r\n"_sl, true));
    }


} }
