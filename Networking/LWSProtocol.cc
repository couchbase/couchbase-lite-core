//
// LWSProtocol.cc
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

#include "LWSProtocol.hh"
#include "LWSUtil.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "c4ExceptionUtils.hh"
#include "fleece/Fleece.hh"
#include <errno.h>


namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;


    static constexpr size_t kWriteChunkSize = 1024;


    LWSProtocol::LWSProtocol(lws *connection)
    :_client(connection)
    {
        retain(this);
    }


    LWSProtocol::~LWSProtocol() {
        DebugAssert(!_client);
    }


    void LWSProtocol::clientCreated(::lws* client) {
        if (client) {
            _client = client;
        } else {
            onConnectionError(c4error_make(LiteCoreDomain, kC4ErrorUnexpectedError,
                                           "libwebsockets unable to create client"_sl));
        }
    }


    int LWSProtocol::_mainDispatch(lws* client, int reason, void *user, void *in, size_t len) {
        Retained<LWSProtocol> retainMe = this;  // prevent destruction during dispatch
        _dispatchResult = 0;
        dispatch(client, reason, user, in, len);
        return _dispatchResult;
    }

    void LWSProtocol::dispatch(lws *client, int reason, void *user, void *in, size_t len) {
        if (_dispatchResult != 0)
            return;

        switch ((lws_callback_reasons)reason) {
                // Client lifecycle:
            case LWS_CALLBACK_WSI_CREATE:
                LogVerbose("**** LWS_CALLBACK_WSI_CREATE (wsi=%p)", client);
                Assert(!_client);
                //if (!_client)
                    _client = client;
                retain(this);
                break;
            case LWS_CALLBACK_WSI_DESTROY:
                LogVerbose("**** LWS_CALLBACK_WSI_DESTROY (wsi=%p)", client);
                Assert(client == _client);
                onDestroy();
                _client = nullptr;
                release(this);
                break;
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
                LogVerbose("**** LWS_CALLBACK_CLIENT_CONNECTION_ERROR");
                onConnectionError(getConnectionError(slice(in, len)));
                break;
            }
            default:
                if (reason < 31 || reason > 36)
                    LogDebug("**** %-s (default)", LWSCallbackName(reason));
                break;
        }
        
        if (_dispatchResult == 0)
            check(lws_callback_http_dummy(client, (lws_callback_reasons)reason, user, in, len));
    }


    bool LWSProtocol::check(int status) {
        if (status == 0)
            return true;
        LogVerbose("    LWSProtocol::check(%d) -- failure(?)", status);
        setDispatchResult(status);
        return false;
    }


    C4Error LWSProtocol::getConnectionError(slice lwsErrorMessage) {
        // Maps substrings of LWS error messages to C4Errors:
        static constexpr struct {slice string; C4ErrorDomain domain; int code;} kMessages[] = {
            {"connect failed"_sl,                 POSIXDomain,     ECONNREFUSED},
            {"ws upgrade unauthorized"_sl,        WebSocketDomain, 401},
            {"CA is not trusted"_sl,              NetworkDomain,   kC4NetErrTLSCertUnknownRoot },
            {"server's cert didn't look good"_sl, NetworkDomain,   kC4NetErrTLSCertUntrusted },
            // TODO: Add more entries
            { }
        };

        int status;
        string statusMessage;
        tie(status,statusMessage) = decodeHTTPStatus();

        C4ErrorDomain domain = WebSocketDomain;
        if (status < 300) {
            domain = NetworkDomain;
            status = kC4NetErrUnknown;
            if (lwsErrorMessage) {
                // LWS does not provide any sort of error code, so just look up the string:
                for (int i = 0; kMessages[i].string; ++i) {
                    if (lwsErrorMessage.containsBytes(kMessages[i].string)) {
                        domain = kMessages[i].domain;
                        status = kMessages[i].code;
                        statusMessage = string(lwsErrorMessage);
                        break;
                    }
                }
            } else {
                statusMessage = "unknown error";
            }
            if (domain == NetworkDomain && status == kC4NetErrUnknown)
                Warn("No error code mapping for libwebsocket message '%.*s'",
                     SPLAT(lwsErrorMessage));
        }
        return c4error_make(domain, status, slice(statusMessage));
    }


#pragma mark - CERTIFICATES:


    alloc_slice LWSProtocol::getCertPublicKey(slice certPEM) {
        alloc_slice paddedPinnedCert;
        if (certPEM[certPEM.size - 1] != 0) {
            paddedPinnedCert = alloc_slice(certPEM.size + 1);
            memcpy((void*)paddedPinnedCert.buf, certPEM.buf, certPEM.size);
            const_cast<uint8_t&>(paddedPinnedCert[certPEM.size]) = 0;
            certPEM = paddedPinnedCert;
        }

        lws_x509_cert* xPinned = nullptr;
        if (0 != lws_x509_create(&xPinned))
            return {};

        alloc_slice key;
        char big[1024];
        auto &info = *(lws_tls_cert_info_results*)big;
        if (0 == lws_x509_parse_from_pem(xPinned, certPEM.buf, certPEM.size) &&
            0 == lws_x509_info(xPinned, LWS_TLS_CERT_INFO_OPAQUE_PUBLIC_KEY,
                               &info,
                               sizeof(big) - sizeof(info) + sizeof(info.ns.name))) {
                key = alloc_slice(&info.ns.name, info.ns.len);
            }
        lws_x509_destroy(&xPinned);
        return key;
    }


    alloc_slice LWSProtocol::getPeerCertPublicKey() {
        char big[1024];
        auto &result = *(lws_tls_cert_info_results*)big;
        if (0 != lws_tls_peer_cert_info(_client, LWS_TLS_CERT_INFO_OPAQUE_PUBLIC_KEY,
                                        &result,
                                        sizeof(big) - sizeof(result) + sizeof(result.ns.name))){
            return {};
        }
        return alloc_slice(&result.ns.name, result.ns.len);
    }


#pragma mark - HTTP HEADERS:


    bool LWSProtocol::addRequestHeader(uint8_t* *dst, uint8_t *end,
                                       const char *header, slice value)
    {
        DebugAssert(header[strlen(header)-1] == ':');
        if (!check(lws_add_http_header_by_name(_client,
                                               (const uint8_t*)header,
                                               (const uint8_t*)value.buf, int(value.size),
                                               dst, end))) {
            LogError("libwebsockets wouldn't let me add enough HTTP headers");
            return false;
        }
        LogVerbose("Added header:  %s %.*s", header, SPLAT(value));
        return true;
    }


    bool LWSProtocol::addContentLengthHeader(uint8_t* *dst, uint8_t *end,
                                             uint64_t contentLength)
    {
        LogVerbose("Added header:  Content-Length: %llu", contentLength);
        return check(lws_add_http_header_content_length(_client, contentLength, dst, end));
    }


    pair<int,string> LWSProtocol::decodeHTTPStatus() {
        char buf[32];
        if (lws_hdr_copy(_client, buf, sizeof(buf) - 1, WSI_TOKEN_HTTP) < 0)
            return {};
        string message;
        auto space = strchr(buf, ' ');
        if (space)
            message = string(space+1);
        return {atoi(buf), message};
    }


    // LWS header names are all lowercase. Convert to title case:
    static void normalizeHeaderCase(string &header) {
        if (header[header.size()-1] == ':')
            header.resize(header.size() - 1);
        bool caps = true;
        for (char &c : header) {
            if (isalpha(c)) {
                if (caps)
                    c = (char) toupper(c);
                caps = false;
            } else {
                caps = true;
            }
        }
    }


    bool LWSProtocol::hasHeader(int /*lws_token_indexes*/ tokenIndex) {
        return lws_hdr_total_length(_client, lws_token_indexes(tokenIndex)) > 0;
    }

    string LWSProtocol::getHeader(int /*lws_token_indexes*/ tokenIndex) {
        char buf[1024];
        int size = lws_hdr_copy(_client, buf, sizeof(buf),
                                lws_token_indexes(tokenIndex));
        if (size < 0) {
            Log("Warning: HTTP response header token%d is too long", tokenIndex);
            return "";
        }
        return string(buf, size);
    }


    string LWSProtocol::getHeaderFragment(int /*lws_token_indexes*/ tokenIndex, unsigned index) {
        char buf[1024];
        int size = lws_hdr_copy_fragment(_client, buf, sizeof(buf),
                                         lws_token_indexes(tokenIndex), index);
        return string(buf, max(size, 0));
    }


    int64_t LWSProtocol::getContentLengthHeader() {
        char buf[30];
        int size = lws_hdr_copy(_client, buf, sizeof(buf), WSI_TOKEN_HTTP_CONTENT_LENGTH);
        if (size <= 0)
            return -1;
        return strtol(buf, nullptr, 10);
    }


    Doc LWSProtocol::encodeHTTPHeaders() {
        // libwebsockets makes it kind of a pain to get the HTTP headers...
        Encoder headers;
        headers.beginDict();

        bool any = false;
        // Enumerate over all the HTTP headers libwebsockets knows about.
        // Sadly, this skips over any nonstandard headers. LWS has no API for enumerating them.
        for (auto token = WSI_TOKEN_HOST; ; token = (enum lws_token_indexes)(token + 1)) {
            if (token == WSI_TOKEN_HTTP)
                continue;
            auto headerName = (const char*) lws_token_to_string(token);
            if (!headerName)
                break;
            if (!*headerName)
                continue;

            string value = getHeader(token);
            if (value.empty())
                continue;

            string header = headerName;
            normalizeHeaderCase(header);

            LogVerbose("      %s: %s", header.c_str(), value.c_str());
            headers.writeKey(slice(header));
            headers.writeString(value);
            any = true;
        }

        headers.endDict();
        if (!any)
            return Doc();
        return headers.finishDoc();
    }


#pragma mark - SENDING DATA:


    void LWSProtocol::callbackOnWriteable() {
        int status = lws_callback_on_writable(_client);
        if (status < 0)
            Warn("lws_callback_on_writable returned %d! (this=%p, wsi=%p)", status, this, _client);
    }


    void LWSProtocol::setDataToSend(fleece::alloc_slice data) {
        synchronized([&](){
            Assert(!_dataToSend);
            _dataToSend = data;
            _unsent = _dataToSend;
            if (_client && _unsent.size > 0)
                callbackOnWriteable();
        });
    }


    void LWSProtocol::sendMoreData(bool asServer) {
        synchronized([&](){
            slice chunk = _unsent.readAtMost(kWriteChunkSize);
            lws_write_protocol type = LWS_WRITE_HTTP;
            if (_unsent.size > 0) {
                Log("--Writing %zu bytes", chunk.size);
            } else {
                Log("--Writing final %zu bytes", chunk.size);
                if (asServer)
                    type = LWS_WRITE_HTTP_FINAL;
            }
            //LogDebug("    %.*s", SPLAT(chunk));

            if (lws_write(_client, (uint8_t*)chunk.buf, chunk.size, type) < 0) {
                Log("  --lws_write failed!");
                setDispatchResult(-1);
            }

            if (_unsent.size > 0) {
                callbackOnWriteable();
            } else {
                _dataToSend = nullslice;
                _unsent = nullslice;
            }
        });
    }


} } 
