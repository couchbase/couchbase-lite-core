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
#include "fleece/Fleece.hh"
#include <optional>

namespace fleece {
    struct slice_istream;
}

namespace litecore { namespace net {
    class ClientSocket;

    /** Implements the core logic of HTTP request/response handling, especially processing
        redirects and authentication challenges, without actually doing any of the networking.
        It just tells you what HTTP request to send and how to interpret the response.
        (Well, actually the \ref sendNextRequest helps do the networking for you if you're using a
        ClientSocket for that.) */
    class HTTPLogic {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        // -------- Setup:

        explicit HTTPLogic(const Address &address,
                           bool handleRedirects =true);
        HTTPLogic(const Address &address,
                  const websocket::Headers &requestHeaders,
                  bool handleRedirects =true);
        ~HTTPLogic();

        /// Specifies the HTTP method to use.
        void setMethod(Method method)               {_method = method;}

        /// Specifies the value of the Content-Length header.
        void setContentLength(uint64_t length)      {_contentLength = length;}

        /// Specifies the value of the User-Agent header to send.
        void setUserAgent(slice ua)                 {_userAgent = ua;}

        /// Sets the WebSocket protocol string to request during the handshake.
        void setWebSocketProtocol(slice p)          {_webSocketProtocol = p; _isWebSocket = true;}

        /// Sets the request headers.
        void setHeaders(const websocket::Headers &requestHeaders);

        /// Interface that provides HTTP cookie storage for an HTTPLogic instance.
        class CookieProvider {
        public:
            virtual ~CookieProvider() = default;
            virtual alloc_slice cookiesForRequest(const Address&) =0;
            virtual void setCookie(const Address&, slice cookieHeader) =0;
        };

        /// Registers an object that manages HTTP cookies.
        void setCookieProvider(CookieProvider *cp)                  {_cookieProvider = cp;}

        // -------- Proxies:

        /// Specifies a proxy server to use.
        void setProxy(std::optional<ProxySpec> p);
        std::optional<ProxySpec> proxy()                            {return _proxy;}

        /// Specifies a default proxy server to use for _all_ future requests.
        static void setDefaultProxy(std::optional<ProxySpec> p)     {sDefaultProxy = p;}
        std::optional<ProxySpec> defaultProxy()                     {return sDefaultProxy;}

        // -------- Request:

        /// The current address/URL, which changes after a redirect.
        /// Don't use this when opening a TCP connection! Use \ref directAddress for that,
        /// because it incorporates proxy settings.
        const Address& address()                    {return _address;}

        /// Sets the "Authorization:" header to send in the request.
        void setAuthHeader(slice authHeader)        {_authHeader = authHeader;}
        slice authHeader()                          {return _authHeader;}

        /// Generates a Basic auth header to pass to \ref setAuthHeader.
        static alloc_slice basicAuth(slice username, slice password);

        /// The actual hostname/port/scheme to connect to.
        /// This is affected by proxy settings and by redirects.
        const Address& directAddress();

        /// Returns an encoded HTTP request (minus the body).
        std::string requestToSend();

        // -------- Response handling:

        /// Possible actions after receiving a response.
        enum Disposition {
            kFailure,       ///< Request failed; give up (for now) and check \ref error.
            kRetry,         ///< Try again with a new socket & request
            kAuthenticate,  ///< Add credentials & retry, or else give up
            kContinue,      ///< Send another request on the _same_ socket (for CONNECT proxy)
            kSuccess        ///< Request succeeded!
        };

        /// Call this when a response is received, then check the return value for what to do next.
        /// @param responseData  All data received, at least up through the double CRLF.
        Disposition receivedResponse(slice responseData);

        /// The HTTP status from the latest response.
        HTTPStatus status()                             {return _httpStatus;}

        /// The HTTP status message from the latest response.
        alloc_slice statusMessage()                     {return _statusMessage;}

        /// The headers of the latest response.
        const websocket::Headers& responseHeaders()     {return _responseHeaders;}

        /// The error status of the latest response.
        C4Error error()                                 {return _error;}

        /// Describes an authentication challenge from the server/proxy.
        struct AuthChallenge {
            AuthChallenge(const Address a, bool fp) :address(a), forProxy(fp) { }
            Address address;        ///< The URL to authenticate to
            bool forProxy;          ///< Is this auth for a proxy?
            std::string type;       ///< Auth type, e.g. "Basic" or "Digest"
            std::string key;        ///< A parameter like "Realm"
            std::string value;      ///< The value of the parameter
        };

        /// If the current disposition is \ref kAuthenticate,
        /// this method will return the details of the auth challenge.
        std::optional<AuthChallenge> authChallenge()    {return _authChallenge;}

        /// Convenience method that uses an HTTPClientSocket to send the request and receive the
        /// response.
        /// The socket must _not_ be connected yet, unless the current disposition is kContinue.
        Disposition sendNextRequest(ClientSocket&, slice body =fleece::nullslice);

        // -------- Utility functions:

        /// Utility function to format an HTTP request or response for display.
        /// Converts CRLF to \n, indents lines, and stops at the end of the headers
        /// (before the blank line).
        static std::string formatHTTP(slice http);

        /** Utility function to parse HTTP headers. Reads header lines from HTTP data until
            it reaches an empty line (CRLFCRLF). On return, \ref httpData will point to any
            data remaining after the empty line. */
        static bool parseHeaders(fleece::slice_istream &httpData, websocket::Headers&);

        /// Given a "Sec-WebSocket-Key" header value, returns the "Sec-WebSocket-Accept" value.
        static std::string webSocketKeyResponse(const std::string &nonce);

    private:
        Disposition failure(C4ErrorDomain domain, int code, slice message =fleece::nullslice);
        Disposition failure(ClientSocket&);
        Disposition failure();
        bool connectingToProxy();
        bool parseStatusLine(fleece::slice_istream &responseData);
        Disposition handleRedirect();
        Disposition handleAuthChallenge(slice headerName, bool forProxy);
        Disposition handleUpgrade();
        Disposition handleResponse();

        Address _address;                               // The current target address (not proxy)
        bool _handleRedirects {false};                  // Should I process redirects or fail?
        Method _method {Method::GET};                   // HTTP method to send
        websocket::Headers _requestHeaders;             // Extra request headers
        int64_t _contentLength {-1};                    // Value of Content-Length header to send
        alloc_slice _userAgent;                         // Value of User-Agent header to send
        alloc_slice _authHeader;                        // Value of Authorization header to send
        CookieProvider* _cookieProvider {nullptr};      // HTTP cookie storage
        std::optional<ProxySpec> _proxy;                // Proxy settings
        std::optional<Address> _proxyAddress;           // Proxy converted to Address

        static std::optional<ProxySpec> sDefaultProxy; // Default proxy settings

        C4Error _error {};                              // Fatal error from last request
        HTTPStatus _httpStatus {HTTPStatus::undefined}; // HTTP status of last request
        alloc_slice _statusMessage;                     // HTTP status message
        websocket::Headers _responseHeaders;            // Response headers
        unsigned _redirectCount {0};                    // Number of redirects handled so far
        bool _authChallenged {false};                   // Received an HTTP auth challenge yet?
        std::optional<AuthChallenge> _authChallenge;    // Latest HTTP auth challenge
        Disposition _lastDisposition {kSuccess};        // Disposition of last request sent

        bool _isWebSocket;                              // Making a WebSocket connection?
        alloc_slice _webSocketProtocol;                 // Value for Sec-WebSocket-Protocol header
        std::string _webSocketNonce;                    // Random nonce for WebSocket handshake
    };

} }
