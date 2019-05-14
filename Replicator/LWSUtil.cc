//
// LWSUtil.cc
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

#include "LWSUtil.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include <errno.h>

namespace litecore { namespace websocket { namespace LWS {
    using namespace std;
    using namespace fleece;

    alloc_slice getCertPublicKey(slice certPEM) {
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


    alloc_slice getPeerCertPublicKey(lws *client) {
        char big[1024];
        auto &result = *(lws_tls_cert_info_results*)big;
        if (0 != lws_tls_peer_cert_info(client, LWS_TLS_CERT_INFO_OPAQUE_PUBLIC_KEY,
                                        &result,
                                        sizeof(big) - sizeof(result) + sizeof(result.ns.name))){
            return {};
        }
        return alloc_slice(&result.ns.name, result.ns.len);
    }


    bool addRequestHeader(lws *_client, uint8_t* *dst, uint8_t *end,
                          const char *header, slice value)
    {
        int i = lws_add_http_header_by_name(_client, (const uint8_t*)header,
                                            (const uint8_t*)value.buf, int(value.size),
                                            dst, end);
        if (i != 0) {
            C4LogToAt(kC4WebSocketLog, kC4LogError,
                      "libwebsockets wouldn't let me add enough HTTP headers");
            return false;
        }
        C4LogToAt(kC4WebSocketLog, kC4LogDebug,
                  "Added header:  %s %.*s", header, SPLAT(value));
        return true;
    }


    pair<int,string> decodeHTTPStatus(lws *client) {
        char buf[32];
        if (lws_hdr_copy(client, buf, sizeof(buf) - 1, WSI_TOKEN_HTTP) < 0)
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


    alloc_slice encodeHTTPHeaders(lws *client) {
        // libwebsockets makes it kind of a pain to get the HTTP headers...
        Encoder headers;
        headers.beginDict();

        char buf[1024];
        bool any = false;
        // Enumerate over all the HTTP headers libwebsockets knows about.
        // Sadly, this skips over any nonstandard headers. LWS has no API for enumerating them.
        for (auto token = WSI_TOKEN_HOST; ; token = (enum lws_token_indexes)(token + 1)) {
            if (token == WSI_TOKEN_HTTP)
                continue;
            auto headerStr = (const char*) lws_token_to_string(token);
            if (!headerStr)
                break;
            if (!*headerStr)
                continue;

            int size = lws_hdr_copy(client, buf, sizeof(buf), token);
            if (size < 0)
                Log("Warning: HTTP response header %s is too long", headerStr);
            if (size <= 0)
                continue;

            string header = headerStr;
            normalizeHeaderCase(header);

            //LogDebug("      %s: %.*s", header, size, buf);
            headers.writeKey(slice(header));
            headers.writeString(slice(buf, size));
            any = true;
        }

        headers.endDict();
        if (!any)
            return {};
        return headers.finish();
    }


    C4Error getConnectionError(lws *client, slice lwsErrorMessage) {
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
        tie(status,statusMessage) = decodeHTTPStatus(client);

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
                C4LogToAt(kC4WebSocketLog, kC4LogWarning,
                          "No error code mapping for libwebsocket message '%.*s'",
                          SPLAT(lwsErrorMessage));
        }
        return c4error_make(domain, status, slice(statusMessage));
    }

} } }
