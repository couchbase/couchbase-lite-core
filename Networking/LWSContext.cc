//
// LWSContext.cc
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

#include "LWSContext.hh"
#include "LWSProtocol.hh"
#include "LWSServer.hh"
#include "LWSUtil.hh"
#include "Address.hh"
#include "StringUtil.hh"
#include "ThreadUtil.hh"
#include "c4ExceptionUtils.hh"

#ifdef TARGET_OS_OSX
#include <Security/Security.h>
#endif


namespace litecore { namespace net {
    using namespace std;
    using namespace fleece;

    /* "various processes involving network roundtrips in the
     * library are protected from hanging forever by timeouts.  If
     * nonzero, this member lets you set the timeout used in seconds.
     * Otherwise a default timeout is used." */
    static constexpr int kTimeoutSecs = 0;

    // Default idle time after which a PING is sent.
    static constexpr short kDefaultPingIntervalSecs = 5 * 60;


    constexpr const char* LWSContext::kBLIPClientProtocol;
    constexpr const char* LWSContext::kHTTPClientProtocol;


    static int protocolCallback(lws *wsi, enum lws_callback_reasons reason,
                                void *user, void *in, size_t len);
    static int serverProtocolCallback(lws *wsi, enum lws_callback_reasons reason,
                                void *user, void *in, size_t len);

    constexpr static const lws_protocols kProtocols[] = {
        { LWSContext::kBLIPClientProtocol,        &protocolCallback, 0, 0},
        { LWSContext::kHTTPClientProtocol,  &protocolCallback, 0, 0},
        { NULL, NULL, 0, 0 }
    };

    constexpr static const lws_protocols kServerProtocols[] = {
        { LWSContext::kHTTPServerProtocol,  &serverProtocolCallback, 0, 0},
        { NULL, NULL, 0, 0 }
    };


#pragma mark - CONTEXT INSTANCE & THREAD:


    static LWSContext* sInstance;

    static C4LogDomain sLWSLog;


    LWSContext& LWSContext::instance() {
        static once_flag once;
        call_once(once, []() {
            sInstance = new LWSContext();
        });
        return *sInstance;
    }
    

    LWSContext::LWSContext() {
        initLogging();

        _info.reset(new lws_context_creation_info);
        memset(_info.get(), 0, sizeof(*_info));
        _info->user = this;
        _info->options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
                         //LWS_SERVER_OPTION_EXPLICIT_VHOSTS |
                         LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
        _info->port = CONTEXT_PORT_NO_LISTEN;
        _info->protocols = kProtocols;
        _info->vhost_name = "LiteCore";  // if we ran a server, this would have to be the hostname
        _info->timeout_secs = kTimeoutSecs;
        _info->ws_ping_pong_interval = kDefaultPingIntervalSecs;

#ifdef LWS_WITH_MBEDTLS
        // mbedTLS does not have a list of root CA certs, so get the system list for it:
        alloc_slice systemRootCertsPEM = getSystemRootCertsPEM();
        _info->client_ssl_ca_mem = systemRootCertsPEM.buf;
        _info->client_ssl_ca_mem_len = (unsigned)systemRootCertsPEM.size;
#endif

        _context = lws_create_context(_info.get());
        if (!_context)
            return;
        LogDebug("Created lws_context %p", _context);
        startEventLoop();
    }


    void LWSContext::startEventLoop() {
        // Create the thread running the context's LWS event loop:
        _thread.reset( new thread([&]() {
            SetThreadName("WebSocket dispatch (Couchbase Lite Core)");
            LogDebug("Libwebsocket event loop starting...");
            while (true) {
                lws_service(_context, 1000);
                // FIXME: The timeout should be longer than 1sec, but long timeouts can lead to
                // long delays in libwebsocket: https://github.com/warmcat/libwebsockets/issues/1582
            }
        }));
    }


    void LWSContext::enqueue(function<void()> fn) {
        _enqueued.push(fn);
        lws_cancel_service(_context);  // triggers LWS_CALLBACK_EVENT_WAIT_CANCELLE
    }


    void LWSContext::dequeue() {
        bool empty;
        auto fn = _enqueued.popNoWaiting(empty);
        if (fn)
            fn();
    }


#pragma mark - CONNECTING AND SERVING:


    void LWSContext::connectClient(LWSProtocol *protocolInstance,
                                   const char *protocolName,
                                   const repl::Address &address,
                                   fleece::slice pinnedServerCert,
                                   const char *method)
    {
        enqueue(bind(&LWSContext::_connectClient, this,
                     protocolInstance, string(protocolName), address,
                     alloc_slice(pinnedServerCert), (method ? string(method) :"")));
    }

    void LWSContext::_connectClient(Retained<LWSProtocol> protocolInstance,
                                    const std::string &protocolName,
                                    repl::Address address,
                                    fleece::alloc_slice pinnedServerCert,
                                    const string &method)
    {
        Log("_connectClient %s %p",
            protocolInstance->className(), protocolInstance.get());
        // Create LWS client and connect:
        string hostname(slice(address.hostname));
        string path(slice(address.path));

        struct lws_client_connect_info info = {};
        info.context = _context;
        info.opaque_user_data = protocolInstance;
        info.port = address.port;
        info.address = hostname.c_str();
        info.host = info.address;
        info.origin = info.address;
        info.path = path.c_str();
        info.local_protocol_name = protocolName.c_str();

        if (method.empty()) {
            info.protocol = protocolName.c_str();  // WebSocket protocol to request on server
        } else {
            info.method = method.c_str();
        }

        if (address.isSecure()) {
            info.ssl_connection = LCCSCF_USE_SSL;
            if (pinnedServerCert)
                info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
        }

        lws* client = lws_client_connect_via_info(&info);
        LogDebug("Created lws %p for %s", client, protocolName.c_str());
        protocolInstance->clientCreated(client);
    }


    void LWSContext::startServer(LWSServer *server,
                                 uint16_t port,
                                 const char *hostname,
                                 const lws_http_mount *mounts)
    {
        enqueue(bind(&LWSContext::_startServer, this,
                     server, port, string(hostname ? hostname : ""), mounts));
    }

    void LWSContext::_startServer(Retained<LWSServer> serverInstance,
                                  uint16_t port,
                                  const string &hostname,
                                  const lws_http_mount *mounts)
    {
        Log("_startServer %s %p on port %u",
            serverInstance->className(), serverInstance.get(), port);
        _info->user = serverInstance;
        _info->port = port;
        _info->protocols = kServerProtocols;
        _info->mounts = mounts;
        _info->vhost_name = kHTTPServerProtocol;
        _info->finalize_arg = serverInstance;
        lws_vhost *vhost = lws_create_vhost(_context, _info.get());
        LogDebug("Created vhost %p for '%s'", vhost, hostname.c_str());
        serverInstance->createdVHost(vhost);
    }


    void LWSContext::stop(LWSServer *serverInstance) {
        enqueue(bind(&LWSContext::_stop, this, serverInstance));
    }

    void LWSContext::_stop(Retained<LWSServer> serverInstance) {
        LogDebug("Stopping %s %p ...", serverInstance->className(), serverInstance.get());
        lws_vhost_destroy(serverInstance->_vhost);
        Log("Stopped %s %p", serverInstance->className(), serverInstance.get());
    }


#pragma mark - CALLBACKS:


    void LWSContext::initLogging() {
        // Configure libwebsocket logging:
        sLWSLog = c4log_getDomain("libwebsockets", true);
        auto logLevel = c4log_getLevel(sLWSLog);
        int flags = LLL_ERR | LLL_WARN | LLL_NOTICE;
        if (logLevel <= kC4LogInfo)
            flags |= LLL_NOTICE;
        if (logLevel <= kC4LogVerbose)
            flags |= LLL_INFO;
        if (logLevel <= kC4LogDebug)
            flags |= LLL_DEBUG;

        lws_set_log_level(flags, [](int level, const char *message) {
            slice msg(message);
            if (msg.size > 0 && msg[msg.size-1] == '\n')
                msg.setSize(msg.size-1);
            if (msg.size == 0)
                return;
            C4LogLevel c4level;
            switch(level) {
                case LLL_ERR:    c4level = kC4LogError;   break;
                case LLL_WARN:   c4level = kC4LogWarning; break;
                case LLL_NOTICE: c4level = kC4LogInfo;    break;
                case LLL_INFO:   c4level = kC4LogVerbose; break;
                default:         c4level = kC4LogDebug;   break;
            }
            C4LogToAt(sLWSLog, c4level, "%.*s", SPLAT(msg));
        });
    }


    // Define className() to make the logging macros work in the static functions below:
    LITECORE_UNUSED static const char* className() {return "LWSContext";}


    static int protocolCallback(lws *wsi, enum lws_callback_reasons reason,
                                void *user, void *in, size_t len)
    {
        try {
            if (reason == LWS_CALLBACK_EVENT_WAIT_CANCELLED) {
                auto context = (LWSContext*)lws_context_user(lws_get_context(wsi));
                context->dequeue();
            }

            auto protocol = (LWSProtocol*) lws_get_opaque_user_data(wsi);
            if (protocol) {
                return protocol->_mainDispatch(wsi, reason, user, in, len);
            } else {
                if (reason != LWS_CALLBACK_EVENT_WAIT_CANCELLED)
                    LogDebug("**** %-s (no client; wsi=%p, user=%p)",
                             LWSCallbackName(reason), wsi, user);
                return lws_callback_http_dummy(wsi, reason, user, in, len);
            }
        } catchError(nullptr);
        return -1;
    }


    static int serverProtocolCallback(lws *wsi, enum lws_callback_reasons reason,
                                      void *user, void *in, size_t len)
    {
        try {
            auto protocol = (LWSProtocol*) lws_get_opaque_user_data(wsi);
            if (protocol)
                return protocolCallback(wsi, reason, user, in, len);

            auto vhost = lws_get_vhost(wsi);
            auto server = vhost ? (LWSServer*) lws_get_vhost_user(vhost) : nullptr;
            if (server) {
                return server->dispatch(wsi, reason, user, in, len);
            } else {
                if (reason != LWS_CALLBACK_EVENT_WAIT_CANCELLED)
                    LogDebug("**** %-s (no vhost protocol; wsi=%p, user=%p)",
                             LWSCallbackName(reason), wsi, user);
                return lws_callback_http_dummy(wsi, reason, user, in, len);
            }
        } catchError(nullptr);
        return -1;
    }


#pragma mark - PLATFORM SPECIFIC:


#ifdef LWS_WITH_MBEDTLS
#ifdef TARGET_OS_OSX
    // Sadly, SecTrustCopyAnchorCertificates() is not available on iOS...
    alloc_slice LWSContext::getSystemRootCertsPEM() {
        CFArrayRef roots;
        OSStatus err = SecTrustCopyAnchorCertificates(&roots);
        if (err)
            return {};
        CFDataRef pemData = nullptr;
        err =  SecItemExport(roots, kSecFormatPEMSequence, kSecItemPemArmour, nullptr, &pemData);
        CFRelease(roots);
        if (err)
            return {};
        alloc_slice pem(CFDataGetBytePtr(pemData), CFDataGetLength(pemData));
        CFRelease(pemData);
        return pem;
    }
#else
    alloc_slice LWSContext::getSystemRootCertsPEM() { return {}; }
#endif
#endif


} }
