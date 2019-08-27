//
// HTTPLogic.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "HTTPTypes.hh"
#include "Headers.hh"
#include "Address.hh"
#include "c4Base.h"
#include "Optional.hh"
#include "fleece/Fleece.hh"

namespace litecore { namespace net {
    class ClientSocket;

    /** Implements the core logic of HTTP request/response handling, especially processing redirects and authentication challenges, without actually doing any of the networking. It just tells you what HTTP request to send and how to interpret the response. */
    class HTTPLogic {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        static bool parseHeaders(slice &httpData, websocket::Headers&);


        // -------- Setup:

        HTTPLogic(const Address &address,
                  const websocket::Headers &requestHeaders,
                  bool handleRedirects =true);
        ~HTTPLogic();

        void setMethod(Method method)               {_method = method;}

        void setContentLength(uint64_t length)      {_contentLength = length;}

        /// Specifies the value of the User-Agent header to send.
        void setUserAgent(slice ua)                 {_userAgent = ua;}

        /// Sets the WebSocket protocol string to request during the handshake.
        void setWebSocketProtocol(slice p)          {_webSocketProtocol = p; _isWebSocket = true;}

        // -------- Proxies:

        enum ProxyType {
            kNoProxy,
            kHTTPProxy,
            //kSOCKSProxy,      // TODO: Add SOCKS support
            //kCONNECTProxy     // TODO: Add CONNECT support
        };

        /// Specifies a proxy server to use.
        void setProxy(ProxyType type, Address addr);

        const Address* proxy()                {return _proxyAddress.get();}
        ProxyType proxyType()                       {return _proxyType;}

        void setProxyAuthHeader(slice authHeader)   {_proxyAuthHeader = authHeader;}

        // -------- Request:

        /// The current address/URL, which changes after a redirect.
        const Address& address()              {return _address;}

        /// Sets the "Authorization:" header to send in the request.
        void setAuthHeader(slice authHeader)        {_authHeader = authHeader;}

        /// Generates a Basic auth header to pass to \ref setAuthHeader.
        static alloc_slice basicAuth(slice username, slice password);

        /// The hostname/port/scheme to connect to. This is affected by proxy settings and by redirects.
        const Address& directAddress();

        /// Returns an encoded HTTP request (minus the body).
        std::string requestToSend();

        // -------- Response handling:

        enum Disposition {
            kFailure,       ///< Request failed; give up (for now) and check \ref error.
            kRetry,         ///< Try again with a new request
            kAuthenticate,  ///< Add credentials & retry, or else give up
            kSuccess        ///< Request succeeded!
        };

        /// Call this when a response is received, then check the return value for what to do next.
        /// @param responseData  All data received, at least up through the double CRLF.
        Disposition receivedResponse(slice responseData);

        /// The HTTP status from the latest response.
        HTTPStatus status()                             {return _httpStatus;}

        /// The HTTP status message from the latest response.
        alloc_slice statusMessage()                     {return _statusMessage;}

        /// The headers of the response.
        const websocket::Headers& responseHeaders()     {return _responseHeaders;}

        /// The error status of the latest response.
        C4Error error()                                 {return _error;}

        struct AuthChallenge {
            AuthChallenge(const Address a, bool fp)
            :address(a), forProxy(fp) { }
            Address address;  ///< The URL to authenticate to
            bool forProxy;          ///< Is this auth for a proxy?
            std::string type;       ///< Auth type, e.g. "Basic" or "Digest"
            std::string key;        ///< A parameter like "Realm"
            std::string value;      ///< The value of the parameter
        };

        /// If \ref receivedResponse returns \ref kAuthenticate, this method will return the details of the auth challenge.
        nonstd::optional<AuthChallenge> authChallenge()     {return _authChallenge;}

        /// Convenience method that uses an XClientSocket to send the request and receive the
        /// response.
        Disposition sendNextRequest(ClientSocket&, slice body =fleece::nullslice);

    private:
        Disposition failure(C4ErrorDomain domain, int code, slice message =fleece::nullslice);
        Disposition failure(ClientSocket&);
        bool parseStatusLine(slice &responseData);
        bool parseResponseHeaders(slice &responseData);
        Disposition handleRedirect();
        Disposition handleAuthChallenge(slice headerName, bool forProxy);
        Disposition handleUpgrade();
        Disposition handleResponse();

        Address _address;                         // The current target address (not proxy)
        bool _handleRedirects {false};
        Method _method {Method::GET};
        websocket::Headers _requestHeaders;
        int64_t _contentLength {-1};
        alloc_slice _userAgent;
        alloc_slice _authHeader;

        std::unique_ptr<Address> _proxyAddress;
        ProxyType _proxyType {kNoProxy};
        alloc_slice _proxyAuthHeader;

        C4Error _error {};
        HTTPStatus _httpStatus {HTTPStatus::undefined};
        alloc_slice _statusMessage;
        websocket::Headers _responseHeaders;
        unsigned _redirectCount {0};
        nonstd::optional<AuthChallenge> _authChallenge;
        Disposition _lastDisposition {kSuccess};

        bool _isWebSocket;
        alloc_slice _webSocketProtocol;
        std::string _webSocketNonce;
    };

} }
