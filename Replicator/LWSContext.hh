//
// LWSContext.hh
//
// Copyright © 2019 Couchbase. All rights reserved.
//

#pragma once
#include "Address.hh"
#include "fleece/slice.hh"
#include <memory>
#include <thread>

// libwebsocket opaque structs:
struct lws;
struct lws_context;
struct lws_context_creation_info;
struct lws_http_mount;
struct lws_vhost;

namespace litecore { namespace websocket {

    class LWSProtocol;
    class LWSServer;


    /** Singleton that manages the libwebsocket context and event thread. */
    class LWSContext {
    public:
        static void initialize();

        // null until initialize() is called
        static LWSContext* instance;

        bool isOpen()                          {return _context != nullptr;}

        ::lws_context* context() const         {return _context;}

        static constexpr const char* kBLIPProtocol = "BLIP_3+CBMobile_2";
        static constexpr const char* kHTTPClientProtocol = "HTTPClient";
        static constexpr const char* kHTTPServerProtocol = "HTTPServer";

        ::lws* connectClient(LWSProtocol *protocolInstance,
                             const char *protocolName,
                             const repl::Address &address,
                             fleece::slice pinnedServerCert,
                             const char *method = nullptr);

        lws_vhost* startServer(LWSServer *server,
                               uint16_t port,
                               const char *hostname,
                               const lws_http_mount *mounts);

    private:
        LWSContext();
        static void logCallback(int level, const char *message);
        static fleece::alloc_slice getSystemRootCertsPEM();
        void startEventLoop();

        std::unique_ptr<lws_context_creation_info> _info;
        ::lws_context*               _context {nullptr};
        std::unique_ptr<std::thread> _thread;
    };

} }
